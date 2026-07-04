#include "ProviderCatalog.h"

#include "HttpClient.h"
#include "JsHelpers.h"

#include <pxr/base/js/json.h>
#include <pxr/base/js/value.h>

#include <cstdlib>

PXR_NAMESPACE_USING_DIRECTIVE

namespace UsdAgent {

namespace {

std::string _TrimTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

bool _EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Parse an OpenAI-style {"data":[{"id":...}]} model list.
std::vector<ModelInfo> _ParseOpenAIModels(const JsValue& root) {
    std::vector<ModelInfo> out;
    if (!root.IsObject()) return out;
    for (const JsValue& m : JsGetArray(root.GetJsObject(), "data")) {
        if (!m.IsObject()) continue;
        const std::string id = JsGetString(m.GetJsObject(), "id");
        if (!id.empty()) out.push_back(ModelInfo{id, /*toolCapable=*/true});
    }
    return out;
}

// Parse Ollama's {"models":[{"name":...,"capabilities":[...]}]} list.
std::vector<ModelInfo> _ParseOllamaTags(const JsValue& root) {
    std::vector<ModelInfo> out;
    if (!root.IsObject()) return out;
    for (const JsValue& m : JsGetArray(root.GetJsObject(), "models")) {
        if (!m.IsObject()) continue;
        const JsObject& obj = m.GetJsObject();
        const std::string name = JsGetString(obj, "name");
        if (name.empty()) continue;
        bool tools = false;
        for (const JsValue& cap : JsGetArray(obj, "capabilities")) {
            if (cap.IsString() && cap.GetString() == "tools") { tools = true; break; }
        }
        out.push_back(ModelInfo{name, tools});
    }
    return out;
}

std::vector<ModelInfo> _GetJsonModels(const std::string&             url,
                                      const std::vector<HttpHeader>& headers,
                                      bool                           ollama,
                                      std::string*                   err) {
    HttpResponse http = HttpGetJson(url, headers, /*timeoutSeconds=*/8);
    if (http.status == 0) {
        if (err) *err = "could not reach " + url + " (" + http.error + ")";
        return {};
    }
    if (http.status < 200 || http.status >= 300) {
        if (err) *err = "HTTP " + std::to_string(http.status) + " from " + url;
        return {};
    }
    JsValue root = JsParseString(http.body);
    std::vector<ModelInfo> models =
        ollama ? _ParseOllamaTags(root) : _ParseOpenAIModels(root);
    if (models.empty() && err) *err = "no models found at " + url;
    return models;
}

} // namespace

const char* ProviderToString(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "anthropic";
        case Provider::OpenAI:    return "openai";
        case Provider::Ollama:    return "ollama";
    }
    return "anthropic";
}

bool ProviderFromString(const std::string& s, Provider& out) {
    if (s == "anthropic") { out = Provider::Anthropic; return true; }
    if (s == "openai")    { out = Provider::OpenAI;    return true; }
    if (s == "ollama")    { out = Provider::Ollama;    return true; }
    return false;
}

const char* ProviderDisplayName(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "Anthropic (Claude)";
        case Provider::OpenAI:    return "OpenAI-compatible";
        case Provider::Ollama:    return "Ollama";
    }
    return "?";
}

const char* ProviderBackendType(Provider p) {
    // Ollama speaks the OpenAI Chat Completions wire format, so it reuses the
    // OpenAI backend; only model discovery differs.
    return (p == Provider::Anthropic) ? "anthropic" : "openai";
}

std::string DefaultBaseUrl(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "";                          // fixed in backend
        case Provider::OpenAI:    return "https://api.openai.com";
        case Provider::Ollama:    return "http://localhost:11434";
    }
    return "";
}

std::string DefaultModel(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "claude-sonnet-4-6";
        case Provider::OpenAI:    return "";
        case Provider::Ollama:    return "";
    }
    return "";
}

const char* ApiKeyEnvVar(Provider p) {
    switch (p) {
        case Provider::Anthropic: return "ANTHROPIC_API_KEY";
        case Provider::OpenAI:    return "OPENAI_API_KEY";
        case Provider::Ollama:    return nullptr;
    }
    return nullptr;
}

bool ProviderRequiresKey(Provider p) {
    return p == Provider::Anthropic || p == Provider::OpenAI;
}

std::string ResolveApiKey(Provider p, const std::string& userKey) {
    if (!userKey.empty()) return userKey;
    if (const char* env = ApiKeyEnvVar(p)) {
        if (const char* v = std::getenv(env)) {
            if (*v) return v;
        }
    }
    return "";
}

std::string OpenAiEndpoint(const std::string& baseUrl, const std::string& resource) {
    std::string base = _TrimTrailingSlash(
        baseUrl.empty() ? DefaultBaseUrl(Provider::OpenAI) : baseUrl);
    // If the base already carries the /v1 segment, don't add a second one.
    const std::string v1 = _EndsWith(base, "/v1") ? "" : "/v1";
    return base + v1 + "/" + resource;
}

std::vector<ModelInfo> FetchModels(Provider           p,
                                   const std::string& baseUrl,
                                   const std::string& apiKey,
                                   std::string*       err) {
    if (err) err->clear();
    const std::string key = ResolveApiKey(p, apiKey);

    switch (p) {
        case Provider::Ollama: {
            std::string base = _TrimTrailingSlash(
                baseUrl.empty() ? DefaultBaseUrl(p) : baseUrl);
            return _GetJsonModels(base + "/api/tags", {}, /*ollama=*/true, err);
        }
        case Provider::OpenAI: {
            std::vector<HttpHeader> headers;
            if (!key.empty()) headers.push_back({"Authorization", "Bearer " + key});
            return _GetJsonModels(OpenAiEndpoint(baseUrl, "models"),
                                  headers, /*ollama=*/false, err);
        }
        case Provider::Anthropic: {
            if (key.empty()) {
                if (err) *err = "set ANTHROPIC_API_KEY (or paste a key) to list models";
                return {};
            }
            std::vector<HttpHeader> headers = {
                {"x-api-key", key},
                {"anthropic-version", "2023-06-01"},
            };
            return _GetJsonModels("https://api.anthropic.com/v1/models",
                                  headers, /*ollama=*/false, err);
        }
    }
    return {};
}

} // namespace UsdAgent
