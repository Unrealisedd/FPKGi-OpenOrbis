#include "package_verify.hpp"
#include "sha256.hpp"

#include <fcntl.h>
#include <cstdio>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fpkgi {

bool verify_package_file(const char* path, uint64_t expected_size, const std::string& expected_sha256,
                         PackageVerification& result, std::string& error) {
    error.clear();
    result = PackageVerification();

    const int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        error = "cannot securely open package file";
        return false;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || info.st_size < 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        error = "package path is not a regular file";
        return false;
    }
    result.actual_size = static_cast<uint64_t>(info.st_size);
    if (result.actual_size != expected_size) {
        close(fd);
        std::ostringstream out;
        out << "package size mismatch: expected " << expected_size << ", got " << result.actual_size;
        error = out.str();
        return false;
    }

    FILE* file = fdopen(fd, "rb");
    if (!file) {
        close(fd);
        error = "cannot open package stream";
        return false;
    }
    unsigned char magic[4] = {0, 0, 0, 0};
    const size_t count = std::fread(magic, 1, sizeof(magic), file);
    const bool read_failed = std::ferror(file) != 0;
    if (read_failed || count != sizeof(magic)) {
        std::fclose(file);
        error = "cannot read package magic";
        return false;
    }
    result.magic_valid = magic[0] == 0x7f && magic[1] == 0x43 &&
                         magic[2] == 0x4e && magic[3] == 0x54;
    if (!result.magic_valid) {
        std::fclose(file);
        error = "package magic mismatch (expected 7F 43 4E 54)";
        return false;
    }

    if (!expected_sha256.empty()) {
        if (std::fseek(file, 0, SEEK_SET) != 0 ||
            !sha256_stream(file, result.actual_sha256, error)) {
            std::fclose(file);
            if (error.empty()) error = "cannot rewind package for hashing";
            return false;
        }
        if (result.actual_sha256 != expected_sha256) {
            std::fclose(file);
            error = "package SHA-256 mismatch";
            return false;
        }
    }
    if (std::fclose(file) != 0) {
        error = "cannot close package file after verification";
        return false;
    }
    return true;
}

} // namespace fpkgi
