#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace fpkgi {

typedef void (*ProgressCallback)(uint64_t downloaded, uint64_t total, void* user);

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    bool initialize(std::string& error);
    void shutdown();
    bool get_text(const std::string& url, size_t max_bytes, std::string& output, std::string& error);
    bool download(const std::string& url, const char* destination, uint64_t expected_size,
                  ProgressCallback callback, void* user, uint64_t& downloaded_bytes,
                  std::string& error);

private:
    int net_pool_;
    int ssl_context_;
    int http_context_;
};

} // namespace fpkgi
