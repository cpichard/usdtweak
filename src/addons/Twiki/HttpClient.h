#pragma once

#include <string>
#include <vector>

namespace UsdAgent {

struct HttpHeader {
    std::string name;
    std::string value;
};

struct HttpResponse {
    // status == 0 if the request never reached the server (DNS/TLS/connection
    // failure). Otherwise the HTTP status code returned by the server.
    int                     status = 0;
    std::string             body;
    std::string             error;   // populated when status == 0
};

// One-shot HTTPS POST with JSON body. Detects http:// vs https:// from url.
//
// On non-2xx responses, .body is still populated with whatever the server
// returned (so callers can extract structured error messages).
HttpResponse HttpPostJson(const std::string&             url,
                          const std::vector<HttpHeader>& headers,
                          const std::string&             jsonBody,
                          int                            timeoutSeconds = 60);

} // namespace UsdAgent
