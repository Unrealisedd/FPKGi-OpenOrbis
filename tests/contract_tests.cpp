#include "catalog.hpp"
#include "package_verify.hpp"
#include "pem_bundle.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool parses(const std::string& json) {
    std::vector<fpkgi::CatalogItem> items;
    std::string error;
    return fpkgi::parse_catalog(json, items, error);
}

void test_catalog_contract() {
    const std::string valid =
        "{\"DATA\":{\"http://lan.example:8080/owner.pkg\":{"
        "\"title_id\":\"FPKG00001\",\"region\":\"ALL\","
        "\"name\":\"Owner Supplied Homebrew\",\"version\":\"01.00\","
        "\"release\":null,\"size\":\"32\",\"min_fw\":\"5.05\","
        "\"cover_url\":\"https://images.example/cover.png\","
        "\"sha256\":\"A0D93F0183F7866E56024651F6EE93318AEE115EC27F2D33D13B9497E8776D01\"}}}";
    std::vector<fpkgi::CatalogItem> items;
    std::string error;
    expect(fpkgi::parse_catalog(valid, items, error), "valid DATA-map catalog parses");
    expect(items.size() == 1, "valid catalog yields one item");
    if (items.size() == 1) {
        expect(items[0].size_bytes == 32, "decimal size is parsed exactly");
        expect(items[0].sha256 == "a0d93f0183f7866e56024651f6ee93318aee115ec27f2d33d13b9497e8776d01",
               "SHA-256 is normalized to lowercase");
    }
    expect(parses("{\"DATA\":{}}"), "empty bundled catalog is valid");

    const char* invalid[] = {
        "{}",
        "{\"DATA\":[],\"extra\":1}",
        "{\"DATA\":{\"https://example.test/a.pkg\":{\"title_id\":\"bad\",\"name\":\"A\",\"version\":\"1\",\"size\":\"1\"}}}",
        "{\"DATA\":{\"https://example.test/a.pkg\":{\"title_id\":\"ABCD00001\",\"name\":\"A\",\"version\":\"1\",\"size\":1}}}",
        "{\"DATA\":{\"https://example.test/a.pkg\":{\"title_id\":\"ABCD00001\",\"name\":\"A\",\"version\":\"1\",\"size\":\"0\"}}}",
        "{\"DATA\":{\"https://example.test/a.pkg\":{\"title_id\":\"ABCD00001\",\"name\":\"A\",\"version\":\"1\",\"size\":\"18446744073709551616\"}}}",
        "{\"DATA\":{\"https://user:pass@example.test/a.pkg\":{\"title_id\":\"ABCD00001\",\"name\":\"A\",\"version\":\"1\",\"size\":\"1\"}}}",
        "{\"DATA\":{\"https://example.test/a.pkg#fragment\":{\"title_id\":\"ABCD00001\",\"name\":\"A\",\"version\":\"1\",\"size\":\"1\"}}}",
        "{\"DATA\":{\"https://example.test/a.pkg\":{\"title_id\":\"ABCD00001\",\"name\":\"A\",\"version\":\"1\",\"size\":\"1\",\"cover_url\":\"//example.test/cover.png\"}}}",
        "{\"DATA\":{\"https://example.test/a.pkg\":{\"title_id\":\"ABCD00001\",\"name\":\"A\",\"version\":\"1\",\"size\":\"1\",\"unknown\":true}}}",
        "{\"DATA\":{\"https://example.test/a.pkg\":{\"title_id\":\"ABCD00001\",\"name\":\"A\",\"version\":\"1\",\"size\":\"1\",\"sha256\":\"1234\"}}}",
        "{\"DATA\":{\"https://example.test/a.pkg\":{\"title_id\":\"ABCD00001\",\"name\":\"A\",\"version\":\"1\",\"size\":\"1\"},\"https://example.test/b.pkg\":{\"title_id\":\"ABCD00001\",\"name\":\"B\",\"version\":\"1\",\"size\":\"2\"}}}",
        "{\"DATA\":{},\"DATA\":{}}",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        expect(!parses(invalid[i]), "malformed catalog metadata is rejected");
}

void test_url_policy() {
    std::string error;
    expect(fpkgi::validate_http_url("http://192.168.1.5:8080/pkg.pkg", error), "explicit LAN HTTP is accepted");
    error.clear();
    expect(fpkgi::validate_http_url("https://example.test/pkg.pkg?tokenless=1", error), "HTTPS URL is accepted");
    error.clear();
    expect(!fpkgi::validate_http_url("https://user@example.test/pkg.pkg", error), "URL credentials are rejected");
    error.clear();
    expect(!fpkgi::validate_http_url("https://example.test/pkg.pkg#x", error), "URL fragments are rejected");
    error.clear();
    expect(!fpkgi::validate_http_url("ftp://example.test/pkg.pkg", error), "non-HTTP scheme is rejected");
    error.clear();
    expect(!fpkgi::validate_http_url("https://example.test:/pkg.pkg", error), "empty URL port is rejected");
    error.clear();
    expect(!fpkgi::validate_http_url("https://example..test/pkg.pkg", error), "malformed host is rejected");
    error.clear();
    expect(!fpkgi::validate_http_url("https://example.test/bad%2.pkg", error), "invalid percent escape is rejected");

    std::string resolved;
    error.clear();
    expect(fpkgi::resolve_redirect_url("https://example.test/a/catalog.json", "../pkg.pkg", resolved, error) &&
               resolved == "https://example.test/a/../pkg.pkg",
           "relative HTTPS redirect is resolved without changing authority");
    error.clear();
    expect(!fpkgi::resolve_redirect_url("https://example.test/a", "//other.test/pkg", resolved, error),
           "protocol-relative redirect is rejected");
    error.clear();
    expect(!fpkgi::resolve_redirect_url("https://example.test/a", "http://example.test/b", resolved, error),
           "HTTPS downgrade is rejected");
    error.clear();
    expect(fpkgi::resolve_redirect_url("http://lan.example/a", "https://example.test/b", resolved, error),
           "HTTP to HTTPS redirect is accepted");
}

void test_canonical_filename() {
    fpkgi::CatalogItem item;
    item.title_id = "FPKG00001";
    item.version = "01.00";
    item.name = "Owner / Package";
    expect(fpkgi::canonical_package_filename(item) == "FPKG00001_01.00_Owner Package.pkg",
           "canonical filename includes title ID, version, and sanitized name");
    item.version = "../../01";
    item.name = "../../owner\\supplied/pkg";
    const std::string filename = fpkgi::canonical_package_filename(item);
    expect(filename.find('/') == std::string::npos && filename.find('\\') == std::string::npos,
           "canonical filename contains no path separators");
    expect(filename.find("..") == std::string::npos, "canonical filename contains no traversal segment");
    expect(filename.compare(0, 10, "FPKG00001_") == 0, "canonical filename remains bound to title ID");
}

void test_pem_bundle() {
    const std::string valid =
        "Mozilla bundle comments\n"
        "-----BEGIN CERTIFICATE-----\n"
        "QUJD\n"
        "-----END CERTIFICATE-----\n";
    std::vector<std::string> certificates;
    std::string error;
    expect(fpkgi::parse_pem_bundle(valid, certificates, error),
           "bounded PEM certificate bundle parses");
    expect(certificates.size() == 1 &&
               certificates[0] == "-----BEGIN CERTIFICATE-----\r\nQUJD\r\n-----END CERTIFICATE-----\r\n",
           "PEM certificate is normalized for native SSL");

    std::string packaged_bundle;
    error.clear();
    const bool packaged_ok =
        fpkgi::load_text_file("assets/cacert.pem", packaged_bundle, error) &&
        fpkgi::parse_pem_bundle(packaged_bundle, certificates, error);
    expect(packaged_ok, "packaged Mozilla CA bundle parses");
    expect(packaged_ok && certificates.size() >= 100,
           "packaged Mozilla CA bundle contains the expected root set");

    error.clear();
    expect(!fpkgi::parse_pem_bundle("-----BEGIN CERTIFICATE-----\nQUJD\n",
                                     certificates, error),
           "incomplete PEM certificate is rejected");
    error.clear();
    expect(!fpkgi::parse_pem_bundle("-----END CERTIFICATE-----\n", certificates, error),
           "unexpected PEM end marker is rejected");
}

void test_package_verification() {
    const char* path = "build/contract-test.pkg";
    const unsigned char payload[] = {
        0x7f, 0x43, 0x4e, 0x54,
        'd','e','t','e','r','m','i','n','i','s','t','i','c','-','t','e','s','t','-','p','a','c','k','a','g','e',
        0x00, 0x01
    };
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(payload), sizeof(payload));
    file.close();

    fpkgi::PackageVerification result;
    std::string error;
    const std::string digest = "a0d93f0183f7866e56024651f6ee93318aee115ec27f2d33d13b9497e8776d01";
    expect(sizeof(payload) == 32, "test package has expected deterministic size");
    expect(fpkgi::verify_package_file(path, sizeof(payload), digest, result, error),
           "exact size, PKG magic, and SHA-256 verify");
    error.clear();
    expect(!fpkgi::verify_package_file(path, sizeof(payload) + 1, digest, result, error),
           "size mismatch is rejected");
    error.clear();
    expect(!fpkgi::verify_package_file(path, sizeof(payload), std::string(64, '0'), result, error),
           "SHA-256 mismatch is rejected");

    std::fstream corrupt(path, std::ios::binary | std::ios::in | std::ios::out);
    const char zero = 0;
    corrupt.write(&zero, 1);
    corrupt.close();
    error.clear();
    expect(!fpkgi::verify_package_file(path, sizeof(payload), std::string(), result, error),
           "non-PKG magic is rejected even without a SHA-256");
    std::remove(path);
}

} // namespace

int main() {
    test_catalog_contract();
    test_url_policy();
    test_canonical_filename();
    test_pem_bundle();
    test_package_verification();
    if (failures) {
        std::cerr << failures << " contract test(s) failed\n";
        return 1;
    }
    std::cout << "All FPKGi Native host contract tests passed\n";
    return 0;
}
