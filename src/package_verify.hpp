#pragma once

#include <stdint.h>
#include <string>

namespace fpkgi {

struct PackageVerification {
    uint64_t actual_size;
    std::string actual_sha256;
    bool magic_valid;

    PackageVerification() : actual_size(0), magic_valid(false) {}
};

bool verify_package_file(const char* path, uint64_t expected_size, const std::string& expected_sha256,
                         PackageVerification& result, std::string& error);

} // namespace fpkgi
