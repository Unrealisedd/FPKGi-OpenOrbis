#include "pem_bundle.hpp"

namespace fpkgi {
namespace {

const size_t kMaxBundleBytes = 1024 * 1024;
const size_t kMaxCertificates = 256;
const char* kBeginCertificate = "-----BEGIN CERTIFICATE-----";
const char* kEndCertificate = "-----END CERTIFICATE-----";

bool is_base64_line(const std::string& line) {
    if (line.empty()) return false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '='))
            return false;
    }
    return true;
}

} // namespace

bool parse_pem_bundle(const std::string& bundle, std::vector<std::string>& certificates,
                      std::string& error) {
    certificates.clear();
    if (bundle.empty() || bundle.size() >= kMaxBundleBytes ||
        bundle.find('\0') != std::string::npos) {
        error = "invalid HTTPS CA bundle size or encoding";
        return false;
    }

    std::string certificate;
    bool has_payload = false;
    size_t position = 0;
    while (position < bundle.size()) {
        const size_t line_end = bundle.find('\n', position);
        const size_t count = line_end == std::string::npos ? bundle.size() - position
                                                           : line_end - position;
        std::string line = bundle.substr(position, count);
        if (!line.empty() && line[line.size() - 1] == '\r') line.resize(line.size() - 1);
        position = line_end == std::string::npos ? bundle.size() : line_end + 1;

        if (line == kBeginCertificate) {
            if (!certificate.empty() || certificates.size() >= kMaxCertificates) {
                error = "invalid HTTPS CA certificate boundary or count";
                return false;
            }
            certificate = line + "\r\n";
            has_payload = false;
        } else if (line == kEndCertificate) {
            if (certificate.empty() || !has_payload) {
                error = "empty or unexpected HTTPS CA certificate";
                return false;
            }
            certificate += line + "\r\n";
            certificates.push_back(certificate);
            certificate.clear();
        } else if (!certificate.empty()) {
            if (!is_base64_line(line)) {
                error = "invalid HTTPS CA certificate encoding";
                return false;
            }
            certificate += line + "\r\n";
            has_payload = true;
        } else if (line.find("BEGIN CERTIFICATE") != std::string::npos ||
                   line.find("END CERTIFICATE") != std::string::npos) {
            error = "malformed HTTPS CA certificate boundary";
            return false;
        }
    }

    if (!certificate.empty() || certificates.empty()) {
        certificates.clear();
        error = "incomplete or empty HTTPS CA bundle";
        return false;
    }
    return true;
}

} // namespace fpkgi
