#pragma once

#include <string>

namespace fpkgi {
bool sha256_file(const char* path, std::string& digest, std::string& error);
}
