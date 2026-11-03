#include "catalog.hpp"
#include "mini_json.hpp"

#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace fpkgi {
namespace {

const size_t kMaxUrlLength = 2048;

struct UrlParts {
    std::string scheme;
    std::string authority;
    size_t path_offset;
};

std::string lower_ascii(std::string value) {
    for (size_t i = 0; i < value.size(); ++i)
        value[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
    return value;
}

bool has_invalid_url_character(const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c <= 0x20 || c > 0x7e) return true;
    }
    return false;
}

bool has_control(const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

bool all_space(const std::string& value) {
    if (value.empty()) return false;
    for (size_t i = 0; i < value.size(); ++i)
        if (value[i] != ' ' && value[i] != '\t') return false;
    return true;
}

bool parse_port(const std::string& value) {
    if (value.empty() || value.size() > 5) return false;
    unsigned port = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
        port = port * 10 + static_cast<unsigned>(value[i] - '0');
    }
    return port > 0 && port <= 65535;
}

bool valid_percent_escapes(const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '%') continue;
        if (i + 2 >= value.size() ||
            !std::isxdigit(static_cast<unsigned char>(value[i + 1])) ||
            !std::isxdigit(static_cast<unsigned char>(value[i + 2]))) return false;
        i += 2;
    }
    return true;
}

bool valid_dns_host(const std::string& host) {
    if (host.empty() || host.size() > 253 || host[0] == '.' || host[host.size() - 1] == '.') return false;
    size_t begin = 0;
    while (begin < host.size()) {
        const size_t dot = host.find('.', begin);
        const size_t end = dot == std::string::npos ? host.size() : dot;
        const size_t length = end - begin;
        if (length == 0 || length > 63 ||
            !std::isalnum(static_cast<unsigned char>(host[begin])) ||
            !std::isalnum(static_cast<unsigned char>(host[end - 1]))) return false;
        for (size_t i = begin + 1; i + 1 < end; ++i) {
            const unsigned char c = static_cast<unsigned char>(host[i]);
            if (!std::isalnum(c) && c != '-') return false;
        }
        if (dot == std::string::npos) break;
        begin = dot + 1;
    }
    return true;
}

bool valid_ipv4_host(const std::string& host) {
    size_t begin = 0;
    int groups = 0;
    while (begin < host.size()) {
        const size_t dot = host.find('.', begin);
        const size_t end = dot == std::string::npos ? host.size() : dot;
        if (end == begin || end - begin > 3) return false;
        unsigned value = 0;
        for (size_t i = begin; i < end; ++i) {
            if (host[i] < '0' || host[i] > '9') return false;
            value = value * 10 + static_cast<unsigned>(host[i] - '0');
        }
        if (value > 255) return false;
        ++groups;
        if (dot == std::string::npos) break;
        begin = dot + 1;
    }
    return groups == 4;
}

bool valid_ipv6_host(const std::string& host) {
    const size_t compression = host.find("::");
    if (compression != std::string::npos && host.find("::", compression + 2) != std::string::npos) return false;
    int groups = 0;
    size_t begin = 0;
    while (begin <= host.size()) {
        const size_t colon = host.find(':', begin);
        const size_t end = colon == std::string::npos ? host.size() : colon;
        const std::string group = host.substr(begin, end - begin);
        if (group.empty()) {
            if (compression == std::string::npos) return false;
        } else if (group.find('.') != std::string::npos) {
            if (colon != std::string::npos || !valid_ipv4_host(group)) return false;
            groups += 2;
        } else {
            if (group.size() > 4) return false;
            for (size_t i = 0; i < group.size(); ++i)
                if (!std::isxdigit(static_cast<unsigned char>(group[i]))) return false;
            ++groups;
        }
        if (colon == std::string::npos) break;
        begin = colon + 1;
    }
    return compression == std::string::npos ? groups == 8 : groups < 8;
}

bool parse_url_parts(const std::string& url, UrlParts& parts, std::string& error) {
    if (url.empty() || url.size() > kMaxUrlLength) {
        error = "URL must be 1-2048 bytes";
        return false;
    }
    if (has_invalid_url_character(url) || url.find('\\') != std::string::npos) {
        error = "URL contains whitespace, a non-ASCII character, or a backslash";
        return false;
    }
    if (!valid_percent_escapes(url)) {
        error = "URL contains an invalid percent escape";
        return false;
    }
    if (url.find('#') != std::string::npos) {
        error = "URL fragments are not allowed";
        return false;
    }

    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        error = "URL must be absolute HTTP or HTTPS";
        return false;
    }
    parts.scheme = lower_ascii(url.substr(0, scheme_end));
    if (parts.scheme != "http" && parts.scheme != "https") {
        error = "URL scheme must be HTTP or HTTPS";
        return false;
    }

    const size_t authority_begin = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?", authority_begin);
    parts.path_offset = authority_end == std::string::npos ? url.size() : authority_end;
    parts.authority = url.substr(authority_begin, parts.path_offset - authority_begin);
    if (parts.authority.empty()) {
        error = "URL authority is empty";
        return false;
    }
    if (parts.authority.find('@') != std::string::npos) {
        error = "URL credentials are not allowed";
        return false;
    }

    std::string host;
    std::string port;
    bool port_specified = false;
    if (parts.authority[0] == '[') {
        const size_t close = parts.authority.find(']');
        if (close == std::string::npos || close == 1) {
            error = "URL has an invalid IPv6 host";
            return false;
        }
        host = parts.authority.substr(1, close - 1);
        if (close + 1 < parts.authority.size()) {
            if (parts.authority[close + 1] != ':') {
                error = "URL authority is malformed";
                return false;
            }
            port_specified = true;
            port = parts.authority.substr(close + 2);
        }
        if (!valid_ipv6_host(host)) {
            error = "URL has an invalid IPv6 host";
            return false;
        }
    } else {
        const size_t colon = parts.authority.rfind(':');
        if (colon != std::string::npos) {
            if (parts.authority.find(':') != colon) {
                error = "IPv6 URL hosts must use brackets";
                return false;
            }
            host = parts.authority.substr(0, colon);
            port_specified = true;
            port = parts.authority.substr(colon + 1);
        } else {
            host = parts.authority;
        }
        const bool numeric_host = host.find_first_not_of("0123456789.") == std::string::npos;
        if ((numeric_host && !valid_ipv4_host(host)) || (!numeric_host && !valid_dns_host(host))) {
            error = "URL host is malformed";
            return false;
        }
    }
    if (port_specified && !parse_port(port)) {
        error = "URL port is invalid";
        return false;
    }
    return true;
}

bool bounded_string(const Json& object, const char* key, bool required, bool nullable,
                    size_t max_length, std::string& out, std::string& error) {
    const Json* value = object.get(key);
    if (!value) {
        if (required) error = std::string("missing required field '") + key + "'";
        return !required;
    }
    if (value->is_null() && nullable) {
        out.clear();
        return true;
    }
    if (!value->is_string()) {
        error = std::string("field '") + key + "' must be a string" + (nullable ? " or null" : "");
        return false;
    }
    out = value->as_string();
    if (out.empty() || out.size() > max_length || has_control(out) || all_space(out)) {
        error = std::string("field '") + key + "' has an invalid length or character";
        return false;
    }
    return true;
}

bool valid_title_id(const std::string& value) {
    if (value.size() != 9) return false;
    for (size_t i = 0; i < 4; ++i)
        if (value[i] < 'A' || value[i] > 'Z') return false;
    for (size_t i = 4; i < 9; ++i)
        if (value[i] < '0' || value[i] > '9') return false;
    return true;
}

bool valid_version(const std::string& value) {
    if (value == "." || value == ".." || value.find("..") != std::string::npos) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') return false;
    }
    return true;
}

bool parse_size(const std::string& text, uint64_t& value) {
    if (text.empty() || text.size() > 20) return false;
    uint64_t result = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const unsigned digit = static_cast<unsigned>(text[i] - '0');
        if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10) return false;
        result = result * 10 + digit;
    }
    if (result == 0) return false;
    value = result;
    return true;
}

bool normalize_sha256(const Json& object, std::string& output, std::string& error) {
    const Json* value = object.get("sha256");
    if (!value) {
        output.clear();
        return true;
    }
    if (!value->is_string() || value->as_string().size() != 64) {
        error = "field 'sha256' must be 64 hexadecimal characters";
        return false;
    }
    output = lower_ascii(value->as_string());
    for (size_t i = 0; i < output.size(); ++i) {
        const char c = output[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            error = "field 'sha256' must be 64 hexadecimal characters";
            return false;
        }
    }
    return true;
}

bool allowed_metadata_key(const std::string& key) {
    return key == "title_id" || key == "region" || key == "name" || key == "version" ||
           key == "release" || key == "size" || key == "min_fw" || key == "cover_url" ||
           key == "sha256";
}

std::string sanitize_component(const std::string& value, size_t max_length, const char* fallback) {
    std::string result;
    bool separator = false;
    for (size_t i = 0; i < value.size() && result.size() < max_length; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        const bool safe = std::isalnum(c) || c == '-' || c == '_' || c == '.';
        if (safe) {
            if (c == '.' && !result.empty() && result[result.size() - 1] == '.') continue;
            result.push_back(static_cast<char>(c));
            separator = false;
        } else if (c == ' ') {
            if (!result.empty() && !separator && result.size() < max_length) result.push_back(' ');
            separator = true;
        } else if (!result.empty() && !separator) {
            result.push_back('_');
            separator = true;
        }
    }
    while (!result.empty()) {
        const char c = result[result.size() - 1];
        if (c != ' ' && c != '.' && c != '_' && c != '-') break;
        result.resize(result.size() - 1);
    }
    size_t begin = 0;
    while (begin < result.size() && (result[begin] == ' ' || result[begin] == '.')) ++begin;
    result = result.substr(begin);
    return result.empty() ? fallback : result;
}

} // namespace

bool validate_http_url(const std::string& url, std::string& error) {
    error.clear();
    UrlParts parts;
    return parse_url_parts(url, parts, error);
}

bool resolve_redirect_url(const std::string& current_url, const std::string& location,
                          std::string& resolved_url, std::string& error) {
    error.clear();
    resolved_url.clear();
    UrlParts current;
    if (!parse_url_parts(current_url, current, error)) return false;
    if (location.empty()) {
        error = "redirect Location is empty";
        return false;
    }
    if (location.compare(0, 2, "//") == 0) {
        error = "protocol-relative redirects are not allowed";
        return false;
    }

    UrlParts next;
    if (location.find("://") != std::string::npos) {
        resolved_url = location;
    } else {
        const size_t first_delimiter = location.find_first_of("/?");
        const size_t colon = location.find(':');
        if (colon != std::string::npos && (first_delimiter == std::string::npos || colon < first_delimiter)) {
            error = "redirect Location has an unsupported scheme";
            return false;
        }
        const std::string origin = current.scheme + "://" + current.authority;
        if (location[0] == '/') {
            resolved_url = origin + location;
        } else if (location[0] == '?') {
            const size_t query = current_url.find('?', current.path_offset);
            const std::string base = query == std::string::npos ? current_url : current_url.substr(0, query);
            resolved_url = base + location;
        } else {
            const size_t query = current_url.find('?', current.path_offset);
            const std::string base = query == std::string::npos ? current_url : current_url.substr(0, query);
            const size_t slash = base.rfind('/');
            resolved_url = (slash == std::string::npos || slash < current.path_offset)
                               ? origin + "/" + location
                               : base.substr(0, slash + 1) + location;
        }
    }
    if (!parse_url_parts(resolved_url, next, error)) return false;
    if (current.scheme == "https" && next.scheme == "http") {
        error = "HTTPS-to-HTTP redirects are not allowed";
        return false;
    }
    return true;
}

bool parse_catalog(const std::string& json, std::vector<CatalogItem>& items, std::string& error) {
    error.clear();
    try {
        const Json root = parse_json(json);
        if (!root.is_object()) {
            error = "catalog root must be an object";
            return false;
        }
        if (root.as_object().size() != 1 || !root.get("DATA")) {
            error = "catalog root must contain only 'DATA'";
            return false;
        }
        const Json* data = root.get("DATA");
        if (!data->is_object()) {
            error = "catalog 'DATA' must be an object keyed by package URL";
            return false;
        }

        std::vector<CatalogItem> parsed;
        std::set<std::string> identities;
        for (std::map<std::string, Json>::const_iterator entry = data->as_object().begin();
             entry != data->as_object().end(); ++entry) {
            CatalogItem item;
            item.pkg_url = entry->first;
            std::string url_error;
            if (!validate_http_url(item.pkg_url, url_error)) {
                error = "invalid package URL: " + url_error;
                return false;
            }
            if (!entry->second.is_object()) {
                error = "metadata for '" + item.pkg_url + "' must be an object";
                return false;
            }
            const Json& metadata = entry->second;
            for (std::map<std::string, Json>::const_iterator field = metadata.as_object().begin();
                 field != metadata.as_object().end(); ++field) {
                if (!allowed_metadata_key(field->first)) {
                    error = "unknown metadata field '" + field->first + "'";
                    return false;
                }
            }

            if (!bounded_string(metadata, "title_id", true, false, 9, item.title_id, error) ||
                !valid_title_id(item.title_id)) {
                if (error.empty()) error = "field 'title_id' must match [A-Z]{4}[0-9]{5}";
                return false;
            }
            if (!bounded_string(metadata, "name", true, false, 96, item.name, error) ||
                !bounded_string(metadata, "version", true, false, 16, item.version, error) ||
                !bounded_string(metadata, "region", false, true, 16, item.region, error) ||
                !bounded_string(metadata, "release", false, true, 32, item.release, error) ||
                !bounded_string(metadata, "min_fw", false, true, 16, item.min_fw, error) ||
                !bounded_string(metadata, "cover_url", false, true, kMaxUrlLength, item.cover_url, error)) {
                return false;
            }
            if (!valid_version(item.version)) {
                error = "field 'version' may contain only letters, digits, '.', '-', and '_' without '..'";
                return false;
            }
            if (!item.cover_url.empty() && !validate_http_url(item.cover_url, url_error)) {
                error = "invalid cover_url: " + url_error;
                return false;
            }

            const Json* size = metadata.get("size");
            if (!size || !size->is_string() || !parse_size(size->as_string(), item.size_bytes)) {
                error = "field 'size' must be a nonzero uint64 decimal string";
                return false;
            }
            if (!normalize_sha256(metadata, item.sha256, error)) return false;

            const std::string identity = item.title_id + "\n" + item.version;
            if (!identities.insert(identity).second) {
                error = "duplicate or conflicting title_id/version identity: " + item.title_id + " " + item.version;
                return false;
            }
            parsed.push_back(item);
        }
        items.swap(parsed);
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::string canonical_package_filename(const CatalogItem& item) {
    const std::string version = sanitize_component(item.version, 24, "0");
    const std::string name = sanitize_component(item.name, 120, "Package");
    return item.title_id + "_" + version + "_" + name + ".pkg";
}

bool load_text_file(const char* path, std::string& content, std::string& error) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) {
        error = std::string("cannot open ") + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    if (!file.good() && !file.eof()) {
        error = std::string("cannot read ") + path;
        return false;
    }
    content = buffer.str();
    return true;
}

} // namespace fpkgi
