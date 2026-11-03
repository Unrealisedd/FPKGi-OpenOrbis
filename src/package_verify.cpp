#include "package_verify.hpp"
#include "sha256.hpp"

#include <cstdio>
#include <sstream>
#include <sys/stat.h>

namespace fpkgi {

bool verify_package_file(const char* path, uint64_t expected_size, const std::string& expected_sha256,
                         PackageVerification& result, std::string& error) {
    error.clear();
    result = PackageVerification();

    struct stat info;
    if (stat(path, &info) != 0 || info.st_size < 0) {
        error = "cannot stat package file";
        return false;
    }
    result.actual_size = static_cast<uint64_t>(info.st_size);
    if (result.actual_size != expected_size) {
        std::ostringstream out;
        out << "package size mismatch: expected " << expected_size << ", got " << result.actual_size;
        error = out.str();
        return false;
    }

    FILE* file = std::fopen(path, "rb");
    if (!file) {
        error = "cannot open package file";
        return false;
    }
    unsigned char magic[4] = {0, 0, 0, 0};
    const size_t count = std::fread(magic, 1, sizeof(magic), file);
    const bool read_failed = std::ferror(file) != 0;
    std::fclose(file);
    if (read_failed || count != sizeof(magic)) {
        error = "cannot read package magic";
        return false;
    }
    result.magic_valid = magic[0] == 0x7f && magic[1] == 0x43 && magic[2] == 0x4e && magic[3] == 0x54;
    if (!result.magic_valid) {
        error = "package magic mismatch (expected 7F 43 4E 54)";
        return false;
    }

    if (!expected_sha256.empty()) {
        if (!sha256_file(path, result.actual_sha256, error)) return false;
        if (result.actual_sha256 != expected_sha256) {
            error = "package SHA-256 mismatch";
            return false;
        }
    }
    return true;
}

} // namespace fpkgi
