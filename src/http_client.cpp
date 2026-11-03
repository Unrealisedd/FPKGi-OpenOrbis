#include "http_client.hpp"
#include "catalog.hpp"

#include <orbis/Http.h>
#include <orbis/Net.h>
#include <orbis/Ssl.h>
#include <orbis/Sysmodule.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <vector>

namespace fpkgi {
namespace {

const char* kUserAgent = "FPKGi-Native/1.00 (OpenOrbis PS4 Homebrew)";
const size_t kNetPoolSize = 64 * 1024;
const size_t kSslPoolSize = 512 * 1024;
const size_t kHttpPoolSize = 512 * 1024;
const int kMaxRedirects = 6;

struct RequestHandles {
    int tmpl;
    int conn;
    int req;
    RequestHandles() : tmpl(-1), conn(-1), req(-1) {}
};

void close_request(RequestHandles& handles) {
    if (handles.req >= 0) sceHttpDeleteRequest(handles.req);
    if (handles.conn >= 0) sceHttpDeleteConnection(handles.conn);
    if (handles.tmpl >= 0) sceHttpDeleteTemplate(handles.tmpl);
    handles.req = handles.conn = handles.tmpl = -1;
}

std::string native_error(const char* operation, int32_t code) {
    std::ostringstream out;
    out << operation << " failed (0x";
    out.setf(std::ios::hex, std::ios::basefield);
    out.width(8);
    out.fill('0');
    out << static_cast<uint32_t>(code) << ")";
    return out.str();
}

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() && (value[begin] == ' ' || value[begin] == '\t')) ++begin;
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t' ||
                           value[end - 1] == '\r' || value[end - 1] == '\n')) --end;
    return value.substr(begin, end - begin);
}

std::string lower_ascii(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c >= 'A' && c <= 'Z') value[i] = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

bool header_value(const char* headers, size_t header_size, const char* name, std::string& value) {
    if (!headers || !header_size) return false;
    const std::string block(headers, header_size);
    std::istringstream stream(block);
    std::string line;
    const std::string wanted = lower_ascii(name);
    while (std::getline(stream, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') line.resize(line.size() - 1);
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        if (lower_ascii(trim(line.substr(0, colon))) == wanted) {
            value = trim(line.substr(colon + 1));
            return true;
        }
    }
    return false;
}

bool open_get(int http_context, const std::string& url, RequestHandles& handles,
              int32_t& status, std::string& redirect, std::string& error) {
    handles.tmpl = sceHttpCreateTemplate(http_context, kUserAgent, ORBIS_HTTP_VERSION_1_1, 1);
    if (handles.tmpl < 0) {
        error = native_error("sceHttpCreateTemplate", handles.tmpl);
        return false;
    }
    handles.conn = sceHttpCreateConnectionWithURL(handles.tmpl, url.c_str(), true);
    if (handles.conn < 0) {
        error = native_error("sceHttpCreateConnectionWithURL", handles.conn);
        return false;
    }
    handles.req = sceHttpCreateRequestWithURL(handles.conn, ORBIS_METHOD_GET, url.c_str(), 0);
    if (handles.req < 0) {
        error = native_error("sceHttpCreateRequestWithURL", handles.req);
        return false;
    }

    const int send_result = sceHttpSendRequest(handles.req, 0, 0);
    if (send_result < 0) {
        error = native_error("sceHttpSendRequest", send_result);
        return false;
    }
    const int status_result = sceHttpGetStatusCode(handles.req, &status);
    if (status_result < 0) {
        error = native_error("sceHttpGetStatusCode", status_result);
        return false;
    }

    if (status >= 300 && status < 400) {
        char* headers = 0;
        size_t header_size = 0;
        const int header_result = sceHttpGetAllResponseHeaders(handles.req, &headers, &header_size);
        if (header_result < 0) {
            error = native_error("sceHttpGetAllResponseHeaders", header_result);
            return false;
        }
        if (!header_value(headers, header_size, "location", redirect)) {
            error = "redirect response did not contain Location";
            return false;
        }
    }
    return true;
}

bool open_final_get(int http_context, const std::string& initial_url, RequestHandles& handles,
                    int32_t& status, std::string& final_url, std::string& error) {
    if (!validate_http_url(initial_url, error)) return false;
    std::string url = initial_url;
    for (int redirects = 0; redirects <= kMaxRedirects; ++redirects) {
        std::string location;
        if (!open_get(http_context, url, handles, status, location, error)) {
            close_request(handles);
            return false;
        }
        if (status < 300 || status >= 400) {
            final_url = url;
            return true;
        }
        close_request(handles);
        if (redirects == kMaxRedirects) {
            error = "too many HTTP redirects";
            return false;
        }
        std::string next;
        if (!resolve_redirect_url(url, location, next, error)) return false;
        url.swap(next);
    }
    error = "too many HTTP redirects";
    return false;
}

std::string status_error(int32_t status) {
    std::ostringstream out;
    out << "HTTP request failed with status " << status;
    return out.str();
}

} // namespace

HttpClient::HttpClient() : net_pool_(-1), ssl_context_(-1), http_context_(-1) {}
HttpClient::~HttpClient() { shutdown(); }

bool HttpClient::initialize(std::string& error) {
    if (http_context_ >= 0) return true;
    int result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
    if (result < 0) {
        error = native_error("load NET module", result);
        return false;
    }
    result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
    if (result < 0) {
        error = native_error("load HTTP module", result);
        return false;
    }
    result = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
    if (result < 0) {
        error = native_error("load SSL module", result);
        return false;
    }
    result = sceNetInit();
    if (result < 0) {
        error = native_error("sceNetInit", result);
        return false;
    }
    net_pool_ = sceNetPoolCreate("FPKGiNativeNet", kNetPoolSize, 0);
    if (net_pool_ < 0) {
        error = native_error("sceNetPoolCreate", net_pool_);
        shutdown();
        return false;
    }
    ssl_context_ = sceSslInit(kSslPoolSize);
    if (ssl_context_ < 0) {
        error = native_error("sceSslInit", ssl_context_);
        shutdown();
        return false;
    }
    http_context_ = sceHttpInit(net_pool_, ssl_context_, kHttpPoolSize);
    if (http_context_ < 0) {
        error = native_error("sceHttpInit", http_context_);
        shutdown();
        return false;
    }
    return true;
}

void HttpClient::shutdown() {
    if (http_context_ >= 0) sceHttpTerm(http_context_);
    if (ssl_context_ >= 0) sceSslTerm();
    if (net_pool_ >= 0) sceNetPoolDestroy(net_pool_);
    http_context_ = ssl_context_ = net_pool_ = -1;
}

bool HttpClient::get_text(const std::string& url, size_t max_bytes, std::string& output,
                          std::string& error) {
    RequestHandles handles;
    int32_t status = 0;
    std::string final_url;
    if (!open_final_get(http_context_, url, handles, status, final_url, error)) return false;
    if (status != 200) {
        error = status_error(status);
        close_request(handles);
        return false;
    }

    output.clear();
    std::vector<char> buffer(32 * 1024);
    for (;;) {
        const int count = sceHttpReadData(handles.req, &buffer[0], static_cast<uint32_t>(buffer.size()));
        if (count < 0) {
            error = native_error("sceHttpReadData", count);
            close_request(handles);
            return false;
        }
        if (count == 0) break;
        if (static_cast<size_t>(count) > max_bytes - output.size()) {
            error = "HTTP response exceeded size limit";
            close_request(handles);
            return false;
        }
        output.append(&buffer[0], static_cast<size_t>(count));
    }
    close_request(handles);
    return true;
}

bool HttpClient::download(const std::string& url, const char* destination, uint64_t expected_size,
                          ProgressCallback callback, void* user, uint64_t& downloaded_bytes,
                          std::string& error) {
    RequestHandles handles;
    int32_t status = 0;
    std::string final_url;
    if (!open_final_get(http_context_, url, handles, status, final_url, error)) return false;
    if (status != 200) {
        error = status_error(status);
        close_request(handles);
        return false;
    }

    int content_length_type = 0;
    size_t content_length = 0;
    if (sceHttpGetResponseContentLength(handles.req, &content_length_type, &content_length) >= 0 &&
        static_cast<uint64_t>(content_length) > expected_size) {
        error = "HTTP response Content-Length exceeded expected package size";
        close_request(handles);
        return false;
    }

    FILE* file = std::fopen(destination, "wb");
    if (!file) {
        error = "cannot create partial package file";
        close_request(handles);
        return false;
    }

    downloaded_bytes = 0;
    std::vector<unsigned char> buffer(256 * 1024);
    bool ok = true;
    for (;;) {
        const int count = sceHttpReadData(handles.req, &buffer[0], static_cast<uint32_t>(buffer.size()));
        if (count < 0) {
            error = native_error("sceHttpReadData", count);
            ok = false;
            break;
        }
        if (count == 0) break;
        const uint64_t amount = static_cast<uint64_t>(count);
        if (downloaded_bytes > expected_size || amount > expected_size - downloaded_bytes) {
            error = "HTTP response body exceeded expected package size";
            ok = false;
            break;
        }
        const size_t written = std::fwrite(&buffer[0], 1, static_cast<size_t>(count), file);
        if (written != static_cast<size_t>(count)) {
            error = "failed to write partial package file";
            ok = false;
            break;
        }
        downloaded_bytes += amount;
        if (callback) callback(downloaded_bytes, expected_size, user);
    }
    if (std::fclose(file) != 0 && ok) {
        error = "failed to flush partial package file";
        ok = false;
    }
    close_request(handles);
    return ok;
}

} // namespace fpkgi
