#include "pkg_installer.hpp"

#include <orbis/AppInstUtil.h>
#include <orbis/Bgft.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace {

// OpenOrbis v0.5.4 declares these BGFT fields as 32-bit, but the PS4 ABI uses
// 64-bit sizes and progress counters. Keep the wire layout explicit.
struct NativeBgftDownloadParam {
    int32_t userId;
    int32_t entitlementType;
    const char* id;
    const char* contentUrl;
    const char* contentExUrl;
    const char* contentName;
    const char* iconPath;
    const char* skuId;
    int32_t option;
    const char* playgoScenarioId;
    const char* releaseDate;
    const char* packageType;
    const char* packageSubType;
    uint64_t packageSize;
};

struct NativeBgftDownloadParamEx {
    NativeBgftDownloadParam params;
    uint32_t slot;
};

struct NativeBgftTaskProgress {
    uint32_t bits;
    int32_t errorResult;
    uint64_t length;
    uint64_t transferred;
    uint64_t lengthTotal;
    uint64_t transferredTotal;
    uint32_t numIndex;
    uint32_t numTotal;
    uint32_t restSec;
    uint32_t restSecTotal;
    int32_t preparingPercent;
    int32_t localCopyPercent;
};

static_assert(offsetof(NativeBgftDownloadParam, packageSize) == 96,
              "unexpected BGFT package-size offset");
static_assert(sizeof(NativeBgftDownloadParam) == 104,
              "unexpected BGFT download-parameter size");
static_assert(sizeof(NativeBgftDownloadParamEx) == 112,
              "unexpected BGFT storage-parameter size");
static_assert(sizeof(NativeBgftTaskProgress) == 64,
              "unexpected BGFT progress size");

} // namespace

extern "C" {
int32_t fpkgiBgftRegisterStorage(NativeBgftDownloadParamEx* params, int32_t* task_id)
    __asm__("sceBgftServiceIntDownloadRegisterTaskByStorageEx");
int32_t fpkgiBgftGetProgress(int32_t task_id, NativeBgftTaskProgress* progress)
    __asm__("sceBgftServiceDownloadGetProgress");
}

namespace fpkgi {
namespace {
const size_t kBgftHeapSize = 1024 * 1024;

struct RequiredModule {
    const char* name;
    const char* label;
};

const RequiredModule kRequiredModules[] = {
    {"libSceSystemService.sprx", "SYSTEM_SERVICE"},
    {"libSceAppInstUtil.sprx", "APP_INST_UTIL"},
    {"libSceSysUtil.sprx", "SYS_UTIL"},
    {"libSceBgft.sprx", "BGFT"},
};

int32_t load_start_module(const char* name) {
    char path[256];
    std::snprintf(path, sizeof(path), "/system/common/lib/%s", name);
    int32_t handle = static_cast<int32_t>(sceKernelLoadStartModule(path, 0, 0, 0, 0, 0));
    if (handle >= 0) return handle;

    const char* sandbox_word = sceKernelGetFsSandboxRandomWord();
    if (sandbox_word && sandbox_word[0]) {
        std::snprintf(path, sizeof(path), "/%s/common/lib/%s", sandbox_word, name);
        return static_cast<int32_t>(sceKernelLoadStartModule(path, 0, 0, 0, 0, 0));
    }
    return handle;
}

std::string code_message(const char* operation, int32_t code) {
    std::ostringstream out;
    out << operation << " failed (0x";
    out.setf(std::ios::hex, std::ios::basefield);
    out.width(8);
    out.fill('0');
    out << static_cast<uint32_t>(code) << ")";
    return out.str();
}

} // namespace

PackageInstaller::PackageInstaller()
    : bgft_heap_(0), bgft_initialized_(false), app_inst_initialized_(false),
      modules_loaded_(false), user_id_(0) {}

PackageInstaller::~PackageInstaller() { shutdown(); }

bool PackageInstaller::load_system_modules(std::string& error, int32_t& error_code) {
    if (modules_loaded_) return true;
    for (size_t i = 0; i < sizeof(kRequiredModules) / sizeof(kRequiredModules[0]); ++i) {
        const int32_t handle = load_start_module(kRequiredModules[i].name);
        if (handle < 0) {
            error_code = handle;
            error = std::string("module load ") + kRequiredModules[i].label + " failed";
            return false;
        }
    }
    modules_loaded_ = true;
    return true;
}

bool PackageInstaller::initialize_app_inst_util(std::string& error, int32_t& error_code) {
    error_code = 0;
    if (app_inst_initialized_) return true;
    if (!load_system_modules(error, error_code)) return false;

    const int32_t result = sceAppInstUtilInitialize();
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilInitialize", result);
        return false;
    }
    app_inst_initialized_ = true;
    return true;
}

bool PackageInstaller::initialize(std::string& error, int32_t& error_code) {
    error_code = 0;
    if (bgft_initialized_ && app_inst_initialized_) return true;
    if (!initialize_app_inst_util(error, error_code)) return false;

    sceUserServiceInitialize(0);
    const int32_t user_result = sceUserServiceGetInitialUser(&user_id_);
    if (user_result < 0) {
        error_code = user_result;
        error = code_message("sceUserServiceGetInitialUser", user_result);
        shutdown();
        return false;
    }

    bgft_heap_ = std::malloc(kBgftHeapSize);
    if (!bgft_heap_) {
        error = "cannot allocate BGFT heap";
        shutdown();
        return false;
    }
    std::memset(bgft_heap_, 0, kBgftHeapSize);
    OrbisBgftInitParams init_params;
    std::memset(&init_params, 0, sizeof(init_params));
    init_params.heap = bgft_heap_;
    init_params.heapSize = kBgftHeapSize;

    const int32_t result = sceBgftServiceIntInit(&init_params);
    if (result != 0) {
        error_code = result;
        error = code_message("sceBgftServiceIntInit", result);
        shutdown();
        return false;
    }
    bgft_initialized_ = true;
    return true;
}

void PackageInstaller::shutdown() {
    if (bgft_initialized_) sceBgftServiceIntTerm();
    if (app_inst_initialized_) sceAppInstUtilTerminate();
    if (bgft_heap_) std::free(bgft_heap_);
    bgft_heap_ = 0;
    bgft_initialized_ = false;
    app_inst_initialized_ = false;
}

bool PackageInstaller::enqueue(const char* pkg_path, const std::string& display_name,
                               const std::string& expected_title_id, uint64_t package_size,
                               int32_t& task_id, std::string& package_title_id,
                               std::string& error, int32_t& error_code) {
    error_code = 0;
    task_id = -1;
    package_title_id.clear();
    if (!initialize(error, error_code)) return false;

    char title_id[16];
    std::memset(title_id, 0, sizeof(title_id));
    int32_t is_app = 0;
    int32_t result = sceAppInstUtilGetTitleIdFromPkg(pkg_path, title_id, &is_app);
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilGetTitleIdFromPkg", result);
        return false;
    }
    package_title_id = title_id;
    if (package_title_id != expected_title_id) {
        error = "package title ID " + package_title_id + " does not match catalog title ID " + expected_title_id;
        return false;
    }

    int32_t exists = 0;
    result = sceAppInstUtilAppExists(title_id, &exists);
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilAppExists", result);
        return false;
    }
    if (exists != 0) {
        error = "title is already installed; overwrite is disabled";
        return false;
    }

    NativeBgftDownloadParamEx params;
    std::memset(&params, 0, sizeof(params));
    params.params.userId = user_id_;
    params.params.entitlementType = 5;
    params.params.id = "";
    params.params.contentUrl = pkg_path;
    params.params.contentExUrl = "";
    params.params.contentName = display_name.c_str();
    params.params.iconPath = "/app0/sce_sys/icon0.png";
    params.params.skuId = "";
    params.params.option = static_cast<int32_t>(ORBIS_BGFT_TASK_OPT_INVISIBLE);
    params.params.playgoScenarioId = "0";
    params.params.releaseDate = "";
    params.params.packageType = "";
    params.params.packageSubType = "";
    params.params.packageSize = package_size;
    params.slot = 0;

    result = fpkgiBgftRegisterStorage(&params, &task_id);
    if (result != 0) {
        error_code = result;
        error = code_message("sceBgftServiceIntDownloadRegisterTaskByStorageEx", result);
        return false;
    }
    result = sceBgftServiceDownloadStartTask(task_id);
    if (result != 0) {
        sceBgftServiceIntDownloadUnregisterTask(task_id);
        error_code = result;
        error = code_message("sceBgftServiceDownloadStartTask", result);
        return false;
    }
    return true;
}

bool PackageInstaller::progress(int32_t task_id, uint64_t& transferred, uint64_t& total,
                                bool& completed, int32_t& task_error, std::string& error,
                                int32_t& error_code) {
    error_code = 0;
    completed = false;
    NativeBgftTaskProgress value;
    std::memset(&value, 0, sizeof(value));
    const int32_t result = fpkgiBgftGetProgress(task_id, &value);
    if (result != 0) {
        error_code = result;
        error = code_message("sceBgftServiceDownloadGetProgress", result);
        return false;
    }
    transferred = value.transferredTotal ? value.transferredTotal : value.transferred;
    total = value.lengthTotal ? value.lengthTotal : value.length;
    task_error = value.errorResult;
    completed = total != 0 && transferred >= total &&
                value.preparingPercent >= 100 && value.localCopyPercent >= 100;
    return true;
}

bool PackageInstaller::query_installation(const std::string& title_id, bool& exists, bool& updating,
                                          std::string& error, int32_t& error_code) {
    error_code = 0;
    exists = false;
    updating = true;
    if (!initialize_app_inst_util(error, error_code)) return false;

    int32_t exists_value = 0;
    int32_t result = sceAppInstUtilAppExists(title_id.c_str(), &exists_value);
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilAppExists", result);
        return false;
    }
    exists = exists_value != 0;
    if (!exists) {
        updating = true;
        return true;
    }
    int32_t updating_value = 0;
    result = sceAppInstUtilAppIsInUpdating(title_id.c_str(), &updating_value);
    if (result != 0) {
        error_code = result;
        error = code_message("sceAppInstUtilAppIsInUpdating", result);
        return false;
    }
    updating = updating_value != 0;
    return true;
}

} // namespace fpkgi
