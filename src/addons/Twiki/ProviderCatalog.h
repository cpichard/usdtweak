#pragma once

#include <string>
#include <vector>

namespace UsdAgent {

// The LLM providers Twiki can talk to.
//   Anthropic - Claude Messages API (AnthropicBackend).
//   OpenAI    - any OpenAI-compatible Chat Completions endpoint (OpenAIBackend):
//               OpenAI itself, llama-server, vLLM, LM Studio, ...
//   Ollama    - a local/remote Ollama server. Chat goes through the same
//               OpenAI-compatible endpoint (OpenAIBackend), but model discovery
//               uses Ollama's native /api/tags (which also reports per-model
//               capabilities, e.g. whether the model supports tools).
enum class Provider { Anthropic, OpenAI, Ollama };

// Stable id used for settings persistence and the backend factory.
const char* ProviderToString(Provider p);          // "anthropic" | "openai" | "ollama"
bool        ProviderFromString(const std::string& s, Provider& out);

// Human-readable label for the UI.
const char* ProviderDisplayName(Provider p);

// Backend type string passed to LLMBackend::Create. Ollama reuses the OpenAI
// backend, so it maps to "openai".
const char* ProviderBackendType(Provider p);

// Sensible default endpoint for a freshly-selected provider. Empty for
// Anthropic (its URL is fixed inside AnthropicBackend).
std::string DefaultBaseUrl(Provider p);

// A reasonable default model id (may be empty: Ollama/OpenAI users pick one).
std::string DefaultModel(Provider p);

// Environment variable consulted for the API key when the UI field is empty.
// nullptr for providers that need no key (Ollama).
const char* ApiKeyEnvVar(Provider p);

// Whether a non-empty key is required to talk to the provider.
bool ProviderRequiresKey(Provider p);

// Effective API key: the user-supplied value if non-empty, else the provider's
// environment variable (if any), else empty.
std::string ResolveApiKey(Provider p, const std::string& userKey);

// Build an OpenAI-style endpoint from a user-entered base URL and a resource
// ("chat/completions" or "models"). Tolerates a trailing slash and a base URL
// that already includes the "/v1" segment (a very common form for llama-server,
// vLLM, LM Studio), so both "http://h:8080" and "http://h:8080/v1" resolve to
// the same ".../v1/<resource>" without doubling. An empty base falls back to
// the public OpenAI endpoint.
std::string OpenAiEndpoint(const std::string& baseUrl, const std::string& resource);

struct ModelInfo {
    std::string id;
    bool        toolCapable = true;   // false only when we positively know it isn't
};

// Best-effort model enumeration for a provider. Performs one blocking HTTP GET.
// On success returns the models (possibly empty); on failure returns {} and
// sets *err (when non-null) to a short human-readable reason. Never throws.
std::vector<ModelInfo> FetchModels(Provider           p,
                                   const std::string& baseUrl,
                                   const std::string& apiKey,
                                   std::string*       err = nullptr);

} // namespace UsdAgent
