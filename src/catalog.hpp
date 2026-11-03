#pragma once

#include <stdint.h>
#include <string>
#include <vector>

namespace fpkgi {

struct CatalogItem {
    CatalogItem() : size_bytes(0) {}

    std::string pkg_url;
    std::string title_id;
    std::string region;
    std::string name;
    std::string version;
    std::string release;
    std::string min_fw;
    std::string cover_url;
    std::string sha256;
    uint64_t size_bytes;
};

bool parse_catalog(const std::string& json, std::vector<CatalogItem>& items, std::string& error);
bool validate_http_url(const std::string& url, std::string& error);
bool resolve_redirect_url(const std::string& current_url, const std::string& location,
                          std::string& resolved_url, std::string& error);
std::string canonical_package_filename(const CatalogItem& item);
bool load_text_file(const char* path, std::string& content, std::string& error);

} // namespace fpkgi
