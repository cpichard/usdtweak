#include "HttpClient.h"

// cpp-httplib relies on these defines being set BEFORE the header is included.
// They are passed by the build system (see src/agent/CMakeLists.txt).
#include <httplib.h>

#include <sys/stat.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>

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

// ---------------------------------------------------------------------------
// Persistent, keep-alive clients keyed by "scheme://host:port".
//
// Previously every request created and destroyed its own httplib::Client,
// opening a brand-new TCP connection each time. The agent's ReAct loop fires
// several requests per turn, so a session churned through many short-lived
// connections; on Windows each closed client socket lingers in TIME_WAIT for
// minutes, and under contention (e.g. another process also hammering the same
// server) the ephemeral-port pool runs dry and connect() starts failing with
// "Could not establish connection".
//
// Reusing one keep-alive client per host collapses a whole session onto a
// single reused socket. httplib transparently reconnects if the server drops
// the idle connection. All HTTP goes through g_clientsMu, which also makes the
// non-thread-safe httplib::Client safe to share (requests are serialized — the
// agent never issues two at once).
std::mutex g_clientsMu;
std::map<std::string, std::unique_ptr<httplib::Client>> g_clients;

httplib::Client& _AcquireClient(const std::string& schemeHost, int timeoutSeconds) {
    std::unique_ptr<httplib::Client>& slot = g_clients[schemeHost];
    if (!slot) {
        slot = std::make_unique<httplib::Client>(schemeHost);
        slot->set_keep_alive(true);
        slot->enable_server_certificate_verification(true);
        slot->set_follow_location(true);
        if (const char* caBundle = _ResolveCaBundle()) {
            slot->set_ca_cert_path(caBundle);
        }
    }
    // Timeouts can differ per call (fast model-list GET vs slow chat POST).
    slot->set_connection_timeout(std::chrono::seconds(timeoutSeconds));
    slot->set_read_timeout      (std::chrono::seconds(timeoutSeconds));
    slot->set_write_timeout     (std::chrono::seconds(timeoutSeconds));
    return *slot;
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

    httplib::Headers httpHeaders;
    for (const HttpHeader& h : headers) {
        httpHeaders.emplace(h.name, h.value);
    }

    std::lock_guard<std::mutex> lock(g_clientsMu);
    httplib::Client& client = _AcquireClient(schemeHost, timeoutSeconds);

    auto result = client.Post(path, httpHeaders, jsonBody, "application/json");
    if (!result) {
        out.error = "request failed: " + httplib::to_string(result.error());
        return out;
    }
    out.status = result->status;
    out.body   = result->body;
    return out;
}

HttpResponse HttpGetJson(const std::string&             url,
                         const std::vector<HttpHeader>& headers,
                         int                            timeoutSeconds) {
    HttpResponse out;

    std::string schemeHost, path;
    if (!_SplitUrl(url, schemeHost, path)) {
        out.error = "malformed URL: " + url;
        return out;
    }

    httplib::Headers httpHeaders;
    for (const HttpHeader& h : headers) {
        httpHeaders.emplace(h.name, h.value);
    }

    std::lock_guard<std::mutex> lock(g_clientsMu);
    httplib::Client& client = _AcquireClient(schemeHost, timeoutSeconds);

    auto result = client.Get(path, httpHeaders);
    if (!result) {
        out.error = "request failed: " + httplib::to_string(result.error());
        return out;
    }
    out.status = result->status;
    out.body   = result->body;
    return out;
}

} // namespace UsdAgent
