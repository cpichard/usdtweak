#include "HttpClient.h"

// cpp-httplib relies on these defines being set BEFORE the header is included.
// They are passed by the build system (see src/agent/CMakeLists.txt).
#include <httplib.h>

#include <sys/stat.h>

#include <chrono>

namespace UsdAgent {

namespace {

// Split "https://host:port/path" into (scheme+host+port, path).
// Returns false on malformed input.
bool _SplitUrl(const std::string& url,
               std::string&       schemeHost,
               std::string&       path) {
    auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return false;
    auto pathStart = url.find('/', schemeEnd + 3);
    if (pathStart == std::string::npos) {
        schemeHost = url;
        path       = "/";
    } else {
        schemeHost = url.substr(0, pathStart);
        path       = url.substr(pathStart);
    }
    return true;
}

bool _FileExists(const char* path) {
    struct stat st;
    return ::stat(path, &st) == 0;
}

// Locate a CA bundle the OS-shipped OpenSSL can use. The OpenSSL we build
// from source via FetchContent has no default trust store, so server cert
// verification fails out of the box. Try the well-known paths in order.
// Override possible via SSL_CERT_FILE env var.
const char* _ResolveCaBundle() {
    if (const char* env = std::getenv("SSL_CERT_FILE")) {
        if (*env && _FileExists(env)) return env;
    }
    static const char* candidates[] = {
        "/etc/ssl/cert.pem",                      // macOS, FreeBSD
        "/etc/ssl/certs/ca-certificates.crt",     // Debian/Ubuntu
        "/etc/pki/tls/certs/ca-bundle.crt",       // RHEL/Fedora
        "/etc/ssl/ca-bundle.pem",                 // openSUSE
        "/opt/homebrew/etc/openssl@3/cert.pem",   // Homebrew on Apple Silicon
        "/usr/local/etc/openssl@3/cert.pem",      // Homebrew on Intel
    };
    for (const char* p : candidates) {
        if (_FileExists(p)) return p;
    }
    return nullptr;
}

} // namespace

HttpResponse HttpPostJson(const std::string&             url,
                          const std::vector<HttpHeader>& headers,
                          const std::string&             jsonBody,
                          int                            timeoutSeconds) {
    HttpResponse out;

    std::string schemeHost, path;
    if (!_SplitUrl(url, schemeHost, path)) {
        out.error = "malformed URL: " + url;
        return out;
    }

    httplib::Client client(schemeHost);
    client.set_connection_timeout(std::chrono::seconds(timeoutSeconds));
    client.set_read_timeout      (std::chrono::seconds(timeoutSeconds));
    client.set_write_timeout     (std::chrono::seconds(timeoutSeconds));
    client.enable_server_certificate_verification(true);
    client.set_follow_location(true);
    if (const char* caBundle = _ResolveCaBundle()) {
        client.set_ca_cert_path(caBundle);
    }

    httplib::Headers httpHeaders;
    for (const HttpHeader& h : headers) {
        httpHeaders.emplace(h.name, h.value);
    }

    auto result = client.Post(path, httpHeaders, jsonBody, "application/json");
    if (!result) {
        out.error = "request failed: " + httplib::to_string(result.error());
        return out;
    }
    out.status = result->status;
    out.body   = result->body;
    return out;
}

} // namespace UsdAgent
