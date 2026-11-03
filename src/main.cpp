#include "catalog.hpp"
#include "http_client.hpp"
#include "package_verify.hpp"
#include "pixel_font.hpp"
#include "pkg_installer.hpp"

#include <SDL2/SDL.h>
#include <orbis/libkernel.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <fcntl.h>
#include <pthread.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

namespace {

using fpkgi::CatalogItem;

const int kWidth = 1920;
const int kHeight = 1080;
const char* kCatalogUrl = "https://raw.githubusercontent.com/Unrealisedd/FPKGi-OpenOrbis/main/catalog/catalog.json";
const char* kBundledCatalog = "/app0/catalog.json";
const char* kDataDirectory = "/data/FPKGi";
const char* kCachedCatalog = "/data/FPKGi/catalog.json";
const char* kDownloadDirectory = "/data/FPKGi/downloads";
const size_t kMaxCatalogBytes = 2 * 1024 * 1024;
const uint64_t kFreeSpaceReserve = 64ULL * 1024ULL * 1024ULL;
const int kInstallPollLimit = 7200;
const uint32_t kInstallPollDelayUs = 500000;
const int kInstallStablePolls = 4;

struct SharedState {
    pthread_mutex_t mutex;
    std::vector<CatalogItem> items;
    fpkgi::HttpClient http;
    bool job_running;
    std::string status;
    uint64_t current;
    uint64_t total;
    int32_t last_error_code;

    SharedState() : job_running(false), status("STARTING"), current(0), total(0), last_error_code(0) {
        pthread_mutex_init(&mutex, 0);
    }
    ~SharedState() {
        http.shutdown();
        pthread_mutex_destroy(&mutex);
    }
};

enum JobType { JobRefresh, JobInstall };

struct JobArgs {
    SharedState* state;
    JobType type;
    CatalogItem item;
};

bool ensure_private_directory(const char* path, std::string& error) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        error = std::string("cannot create ") + path + ": " + std::strerror(errno);
        return false;
    }
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        error = std::string(path) + " is not a real directory";
        return false;
    }
    if (chmod(path, 0700) != 0) {
        error = std::string("cannot secure ") + path + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

bool ensure_directories(std::string& error) {
    return ensure_private_directory(kDataDirectory, error) &&
           ensure_private_directory(kDownloadDirectory, error);
}

void set_status(SharedState* state, const std::string& status, bool running, int32_t error_code = 0) {
    pthread_mutex_lock(&state->mutex);
    state->status = status;
    state->last_error_code = error_code;
    if (!running) {
        state->current = 0;
        state->total = 0;
    }
    pthread_mutex_unlock(&state->mutex);
}

void finish_job(SharedState* state) {
    pthread_mutex_lock(&state->mutex);
    state->job_running = false;
    state->current = 0;
    state->total = 0;
    pthread_mutex_unlock(&state->mutex);
}

void progress_callback(uint64_t current, uint64_t total, void* user) {
    SharedState* state = static_cast<SharedState*>(user);
    pthread_mutex_lock(&state->mutex);
    state->current = current;
    state->total = total;
    pthread_mutex_unlock(&state->mutex);
}

bool atomic_save_catalog(const std::string& value, std::string& error) {
    const std::string temporary = std::string(kCachedCatalog) + ".tmp";
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        error = "cannot securely create catalog temporary file";
        return false;
    }
    FILE* file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        error = "cannot open catalog temporary stream";
        return false;
    }
    bool ok = std::fwrite(value.data(), 1, value.size(), file) == value.size();
    if (ok && std::fflush(file) != 0) ok = false;
    if (ok && fsync(fileno(file)) != 0) ok = false;
    if (std::fclose(file) != 0) ok = false;
    if (!ok) {
        remove(temporary.c_str());
        error = "cannot flush catalog temporary file";
        return false;
    }
    if (rename(temporary.c_str(), kCachedCatalog) != 0) {
        remove(temporary.c_str());
        error = "cannot atomically replace catalog cache";
        return false;
    }
    return true;
}

bool file_exists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

bool preflight_space(uint64_t expected_size, bool& known, uint64_t& available, std::string& error) {
    known = false;
    available = 0;
    struct statvfs info;
    std::memset(&info, 0, sizeof(info));
    if (statvfs(kDownloadDirectory, &info) != 0 || info.f_frsize == 0) return true;
    known = true;
    if (info.f_frsize != 0 && static_cast<uint64_t>(info.f_bavail) >
                                  std::numeric_limits<uint64_t>::max() / static_cast<uint64_t>(info.f_frsize)) {
        available = std::numeric_limits<uint64_t>::max();
    } else {
        available = static_cast<uint64_t>(info.f_bavail) * static_cast<uint64_t>(info.f_frsize);
    }
    if (expected_size > std::numeric_limits<uint64_t>::max() - kFreeSpaceReserve ||
        available < expected_size + kFreeSpaceReserve) {
        std::ostringstream out;
        out << "insufficient /data space: need package plus 67108864-byte reserve; available " << available;
        error = out.str();
        return false;
    }
    return true;
}

std::string human_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    if (unit == 0) out << bytes;
    else {
        out.setf(std::ios::fixed);
        out.precision(1);
        out << value;
    }
    out << ' ' << units[unit];
    return out.str();
}

void refresh_catalog(SharedState* state) {
    set_status(state, "REFRESHING CATALOG", true);
    std::string error;
    if (!state->http.initialize(error)) {
        set_status(state, "CATALOG REFRESH FAILED - CURRENT LIST KEPT: " + error, false);
        return;
    }
    std::string json;
    if (!state->http.get_text(kCatalogUrl, kMaxCatalogBytes, json, error)) {
        set_status(state, "CATALOG REFRESH FAILED - CURRENT LIST KEPT: " + error, false);
        return;
    }
    std::vector<CatalogItem> parsed;
    if (!fpkgi::parse_catalog(json, parsed, error)) {
        set_status(state, "CATALOG INVALID - CURRENT LIST KEPT: " + error, false);
        return;
    }
    if (!atomic_save_catalog(json, error)) {
        set_status(state, "CATALOG SAVE FAILED - CURRENT LIST KEPT: " + error, false);
        return;
    }

    pthread_mutex_lock(&state->mutex);
    state->items.swap(parsed);
    state->status = "CATALOG UPDATED";
    state->current = state->total = 0;
    state->last_error_code = 0;
    pthread_mutex_unlock(&state->mutex);
}

void install_package(SharedState* state, const CatalogItem& item) {
    std::string error;
    const std::string filename = fpkgi::canonical_package_filename(item);
    const std::string pkg_path = std::string(kDownloadDirectory) + "/" + filename;
    const std::string partial_path = pkg_path + ".part";

    fpkgi::PackageVerification verification;
    if (file_exists(pkg_path)) {
        set_status(state, "VERIFYING EXISTING PACKAGE", true);
        if (!fpkgi::verify_package_file(pkg_path.c_str(), item.size_bytes, item.sha256, verification, error)) {
            set_status(state, "EXISTING PACKAGE INVALID - FILE KEPT: " + error, false);
            return;
        }
        set_status(state, "EXISTING PACKAGE VERIFIED", true);
    } else {
        bool space_known = false;
        uint64_t available = 0;
        if (!preflight_space(item.size_bytes, space_known, available, error)) {
            set_status(state, "DOWNLOAD PREFLIGHT FAILED: " + error, false);
            return;
        }
        set_status(state, space_known ? "DOWNLOADING " + item.name
                                      : "FREE SPACE UNKNOWN; DOWNLOADING " + item.name,
                   true);
        if (!state->http.initialize(error)) {
            set_status(state, "NETWORK INIT FAILED: " + error, false);
            return;
        }
        uint64_t downloaded = 0;
        if (!state->http.download(item.pkg_url, partial_path.c_str(), item.size_bytes,
                           progress_callback, state, downloaded, error)) {
            set_status(state, "DOWNLOAD FAILED - PARTIAL KEPT: " + error, false);
            return;
        }

        set_status(state, "VERIFYING PACKAGE", true);
        if (!fpkgi::verify_package_file(partial_path.c_str(), item.size_bytes, item.sha256,
                                        verification, error)) {
            set_status(state, "PACKAGE VERIFY FAILED - PARTIAL KEPT: " + error, false);
            return;
        }
        if (file_exists(pkg_path)) {
            set_status(state, "FINAL PACKAGE APPEARED DURING DOWNLOAD - BOTH FILES KEPT", false);
            return;
        }
        if (rename(partial_path.c_str(), pkg_path.c_str()) != 0) {
            set_status(state, "PACKAGE VERIFIED BUT FINAL RENAME FAILED - PARTIAL KEPT", false);
            return;
        }
    }

    progress_callback(0, 0, state);
    set_status(state, "SUBMITTING VERIFIED PACKAGE TO BGFT", true);
    fpkgi::PackageInstaller installer;
    int32_t error_code = 0;
    int32_t task_id = -1;
    std::string package_title_id;
    if (!installer.enqueue(pkg_path.c_str(), item.name, item.title_id, item.size_bytes,
                           task_id, package_title_id, error, error_code)) {
        set_status(state, "BGFT START FAILED - PKG KEPT: " + error, false, error_code);
        return;
    }

    {
        std::ostringstream status;
        status << "BGFT TASK " << task_id << " INSTALLING " << package_title_id;
        set_status(state, status.str(), true);
    }

    int stable_polls = 0;
    for (int poll = 0; poll < kInstallPollLimit; ++poll) {
        uint64_t transferred = 0;
        uint64_t total = 0;
        bool bgft_completed = false;
        int32_t task_error = 0;
        if (!installer.progress(task_id, transferred, total, bgft_completed, task_error,
                                error, error_code)) {
            set_status(state, "BGFT PROGRESS FAILED - PKG KEPT: " + error, false, error_code);
            return;
        }
        progress_callback(transferred, total, state);
        if (task_error != 0) {
            set_status(state, "BGFT TASK FAILED - PKG KEPT", false, task_error);
            return;
        }

        bool exists = false;
        bool updating = true;
        if (!installer.query_installation(package_title_id, exists, updating, error, error_code)) {
            set_status(state, "INSTALL CHECK FAILED - PKG KEPT: " + error, false, error_code);
            return;
        }
        if (bgft_completed && exists && !updating) {
            ++stable_polls;
            if (stable_polls >= kInstallStablePolls) {
                if (remove(pkg_path.c_str()) != 0) {
                    set_status(state, "INSTALL VERIFIED - LOCAL PKG DELETE FAILED", false);
                } else {
                    set_status(state, "INSTALL COMPLETE - VERIFIED LOCAL PKG DELETED", false);
                }
                return;
            }
        } else {
            stable_polls = 0;
        }
        sceKernelUsleep(kInstallPollDelayUs);
    }
    set_status(state, "INSTALL VERIFY TIMEOUT - PKG KEPT", false);
}

void* job_main(void* raw) {
    JobArgs* args = static_cast<JobArgs*>(raw);
    SharedState* state = args->state;
    const JobType type = args->type;
    const CatalogItem item = args->item;
    delete args;

    if (type == JobRefresh) refresh_catalog(state);
    else install_package(state, item);
    finish_job(state);
    return 0;
}

bool start_job(SharedState& state, JobType type, const CatalogItem* item) {
    pthread_mutex_lock(&state.mutex);
    if (state.job_running) {
        pthread_mutex_unlock(&state.mutex);
        return false;
    }
    state.job_running = true;
    state.current = state.total = 0;
    state.last_error_code = 0;
    state.status = type == JobRefresh ? "STARTING CATALOG REFRESH" : "STARTING DOWNLOAD";
    pthread_mutex_unlock(&state.mutex);

    JobArgs* args = new JobArgs;
    args->state = &state;
    args->type = type;
    if (item) args->item = *item;
    pthread_t thread;
    if (pthread_create(&thread, 0, job_main, args) != 0) {
        delete args;
        set_status(&state, "COULD NOT CREATE WORKER THREAD", false);
        finish_job(&state);
        return false;
    }
    pthread_detach(thread);
    return true;
}

void load_initial_catalog(SharedState& state) {
    std::string json;
    std::string error;
    std::vector<CatalogItem> parsed;
    if (fpkgi::load_text_file(kCachedCatalog, kMaxCatalogBytes, json, error) &&
        fpkgi::parse_catalog(json, parsed, error)) {
        state.items.swap(parsed);
        state.status = "CACHED CATALOG LOADED";
        return;
    }
    json.clear();
    parsed.clear();
    if (fpkgi::load_text_file(kBundledCatalog, kMaxCatalogBytes, json, error) &&
        fpkgi::parse_catalog(json, parsed, error)) {
        state.items.swap(parsed);
        state.status = "BUNDLED EMPTY CATALOG LOADED";
    } else {
        state.status = "NO VALID LOCAL CATALOG";
    }
}

void fill(SDL_Renderer* renderer, int x, int y, int w, int h, SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(renderer, &rect);
}

std::string truncate_text(const std::string& value, size_t max_chars) {
    if (value.size() <= max_chars) return value;
    if (max_chars < 4) return value.substr(0, max_chars);
    return value.substr(0, max_chars - 3) + "...";
}

void render(SDL_Renderer* renderer, SharedState& state, int selected) {
    const SDL_Color background = {13, 18, 28, 255};
    const SDL_Color panel = {24, 32, 46, 255};
    const SDL_Color selected_color = {45, 112, 196, 255};
    const SDL_Color white = {240, 244, 250, 255};
    const SDL_Color muted = {150, 164, 184, 255};
    const SDL_Color accent = {82, 193, 170, 255};
    fill(renderer, 0, 0, kWidth, kHeight, background);
    fill(renderer, 0, 0, kWidth, 120, panel);
    fpkgi::draw_text(renderer, 70, 38, 7, "FPKGI NATIVE", white);
    fpkgi::draw_text(renderer, 1270, 48, 3,
                     "X DOWNLOAD+INSTALL   TRIANGLE REFRESH   CIRCLE EXIT WHEN IDLE", muted);

    std::vector<CatalogItem> items;
    std::string status;
    uint64_t current = 0;
    uint64_t total = 0;
    int32_t error_code = 0;
    pthread_mutex_lock(&state.mutex);
    items = state.items;
    status = state.status;
    current = state.current;
    total = state.total;
    error_code = state.last_error_code;
    pthread_mutex_unlock(&state.mutex);

    if (items.empty()) {
        fpkgi::draw_text(renderer, 120, 260, 5, "NO PACKAGES IN THE VALIDATED CATALOG", white);
        fpkgi::draw_text(renderer, 120, 330, 3, "PRESS TRIANGLE TO REFRESH THE REMOTE CATALOG", muted);
    } else {
        const int visible = 10;
        int first = std::max(0, selected - visible + 1);
        if (selected < first) first = selected;
        for (int row = 0; row < visible && first + row < static_cast<int>(items.size()); ++row) {
            const int index = first + row;
            const int y = 150 + row * 78;
            fill(renderer, 70, y, 1780, 66, index == selected ? selected_color : panel);
            fpkgi::draw_text(renderer, 100, y + 8, 4, truncate_text(items[index].name, 48), white);
            fpkgi::draw_text(renderer, 102, y + 42, 2, items[index].title_id, index == selected ? white : muted);
            const std::string meta = "V" + items[index].version + "  " + human_bytes(items[index].size_bytes);
            fpkgi::draw_text(renderer, 1440, y + 22, 3, meta, index == selected ? white : muted);
        }
    }

    fill(renderer, 0, 940, kWidth, 140, panel);
    fpkgi::draw_text(renderer, 70, 968, 3, truncate_text(status, 95), white);
    if (error_code) {
        std::ostringstream code;
        code << "ERROR 0X";
        code.setf(std::ios::hex, std::ios::basefield);
        code.width(8);
        code.fill('0');
        code << static_cast<uint32_t>(error_code);
        fpkgi::draw_text(renderer, 1510, 968, 3, code.str(), white);
    }
    if (current || total) {
        const int bar_x = 70;
        const int bar_y = 1025;
        const int bar_w = 1780;
        const int bar_h = 20;
        fill(renderer, bar_x, bar_y, bar_w, bar_h, background);
        int filled = 0;
        if (total) filled = static_cast<int>((static_cast<double>(current) / static_cast<double>(total)) * bar_w);
        fill(renderer, bar_x, bar_y, std::max(4, std::min(bar_w, filled)), bar_h, accent);
        std::string progress = human_bytes(current);
        if (total) {
            const int percent = static_cast<int>((static_cast<double>(current) / static_cast<double>(total)) * 100.0);
            std::ostringstream progress_text;
            progress_text << percent << "%   " << progress << " / " << human_bytes(total);
            progress = progress_text.str();
        }
        fpkgi::draw_text(renderer, 1370, 995, 3, progress, muted);
    }
}

} // namespace

int main(int, char**) {
    setvbuf(stdout, 0, _IONBF, 0);
    umask(0077);
    std::string storage_error;
    if (!ensure_directories(storage_error)) {
        std::fprintf(stderr, "FPKGi storage initialization failed: %s\n", storage_error.c_str());
        return 4;
    }
    SharedState state;
    load_initial_catalog(state);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) return 1;
    SDL_Window* window = SDL_CreateWindow("FPKGi Native", SDL_WINDOWPOS_UNDEFINED,
                                          SDL_WINDOWPOS_UNDEFINED, kWidth, kHeight, 0);
    if (!window) return 2;
    SDL_Surface* surface = SDL_GetWindowSurface(window);
    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    if (!renderer) return 3;
    SDL_Joystick* joystick = SDL_NumJoysticks() > 0 ? SDL_JoystickOpen(0) : 0;

    start_job(state, JobRefresh, 0);
    int selected = 0;
    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            bool up = false;
            bool down = false;
            bool choose = false;
            bool refresh = false;
            bool quit = false;
            if (event.type == SDL_QUIT) quit = true;
            if (event.type == SDL_KEYDOWN) {
                up = event.key.keysym.sym == SDLK_UP;
                down = event.key.keysym.sym == SDLK_DOWN;
                choose = event.key.keysym.sym == SDLK_RETURN;
                refresh = event.key.keysym.sym == SDLK_r;
                quit = event.key.keysym.sym == SDLK_ESCAPE;
            } else if (event.type == SDL_JOYHATMOTION) {
                up = (event.jhat.value & SDL_HAT_UP) != 0;
                down = (event.jhat.value & SDL_HAT_DOWN) != 0;
            } else if (event.type == SDL_JOYBUTTONDOWN) {
                choose = event.jbutton.button == 0;
                quit = event.jbutton.button == 1;
                refresh = event.jbutton.button == 3;
            }

            pthread_mutex_lock(&state.mutex);
            const int count = static_cast<int>(state.items.size());
            const bool busy = state.job_running;
            CatalogItem chosen;
            if (count && selected >= 0 && selected < count) chosen = state.items[selected];
            pthread_mutex_unlock(&state.mutex);

            if (up && count) selected = (selected + count - 1) % count;
            if (down && count) selected = (selected + 1) % count;
            if (refresh && !busy) start_job(state, JobRefresh, 0);
            if (choose && count && !busy) start_job(state, JobInstall, &chosen);
            if (quit && !busy) running = false;
        }

        pthread_mutex_lock(&state.mutex);
        const int count = static_cast<int>(state.items.size());
        pthread_mutex_unlock(&state.mutex);
        if (count == 0) selected = 0;
        else if (selected >= count) selected = count - 1;

        render(renderer, state, selected);
        SDL_UpdateWindowSurface(window);
        SDL_Delay(16);
    }

    if (joystick) SDL_JoystickClose(joystick);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
