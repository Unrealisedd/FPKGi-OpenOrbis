#pragma once

#include <stdint.h>
#include <string>

namespace fpkgi {

class PackageInstaller {
public:
    PackageInstaller();
    ~PackageInstaller();

    bool initialize(std::string& error, int32_t& error_code);
    void shutdown();

    bool enqueue(const char* pkg_path, const std::string& display_name,
                 const std::string& expected_title_id, uint64_t package_size,
                 int32_t& task_id, std::string& package_title_id,
                 std::string& error, int32_t& error_code);
    bool progress(int32_t task_id, uint64_t& transferred, uint64_t& total,
                  bool& completed, int32_t& task_error, std::string& error,
                  int32_t& error_code);
    bool query_installation(const std::string& title_id, bool& exists, bool& updating,
                            std::string& error, int32_t& error_code);

private:
    bool load_system_modules(std::string& error, int32_t& error_code);
    bool initialize_app_inst_util(std::string& error, int32_t& error_code);

    void* bgft_heap_;
    bool bgft_initialized_;
    bool app_inst_initialized_;
    bool modules_loaded_;
    int32_t user_id_;
};

} // namespace fpkgi
