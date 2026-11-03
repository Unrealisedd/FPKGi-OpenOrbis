#pragma once

#include <stdio.h>
#include <string>

namespace fpkgi {
bool sha256_stream(FILE* file, std::string& digest, std::string& error);
}
