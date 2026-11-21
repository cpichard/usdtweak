#pragma once

#include <functional>
#include <string>
#include <vector>

#include <llm_retry.hpp>

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

// One-shot GET. Used to enumerate models from a provider (e.g. Ollama's
// /api/tags or an OpenAI-compatible /v1/models). Same URL/scheme handling and
// error semantics as HttpPostJson.
HttpResponse HttpGetJson(const std::string&             url,
                         const std::vector<HttpHeader>& headers,
                         int                            timeoutSeconds = 30);

// Runs post() and retries 408/429/5xx/529 with exponential backoff (llm-retry).
// Transport failures (status 0) are returned as-is. Returns the last response.
HttpResponse PostWithRetry(const std::function<HttpResponse()>& post,
                           const llm::RetryConfig&              config = {});

} // namespace UsdAgent
