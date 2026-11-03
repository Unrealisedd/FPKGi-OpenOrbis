#pragma once

#include <string>
#include <vector>

namespace fpkgi {

bool parse_pem_bundle(const std::string& bundle, std::vector<std::string>& certificates,
                      std::string& error);

} // namespace fpkgi
