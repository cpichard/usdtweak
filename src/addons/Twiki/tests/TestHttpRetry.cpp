// PostWithRetry: a 429/529 is retried, a 400 is not, a transport error is not.
// No network: post() is a scripted fake.

#include "HttpClient.h"

#include <cstdio>
#include <vector>

using namespace UsdAgent;

namespace {

int g_failures = 0;

#define CHECK(cond) do {                                                       \
    if (!(cond)) {                                                             \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        ++g_failures;                                                          \
    }                                                                          \
} while (0)

llm::RetryConfig FastConfig() {
    llm::RetryConfig c;
    c.base_delay_ms = 1.0;
    c.max_delay_ms  = 2.0;
    return c;
}

// Returns the scripted statuses in order, counting calls.
struct Scripted {
    std::vector<int> statuses;
    int              calls = 0;
    HttpResponse operator()() {
        HttpResponse r;
        r.status = statuses[calls++];
        r.body   = "body" + std::to_string(r.status);
        return r;
    }
};

} // namespace

int main() {
    {   // 429 then 529 then 200: the turn survives.
        Scripted s{{429, 529, 200}};
        HttpResponse r = PostWithRetry([&] { return s(); }, FastConfig());
        CHECK(r.status == 200);
        CHECK(s.calls == 3);
    }
    {   // Overloaded on every attempt: the last error comes back unchanged.
        Scripted s{{529, 529, 529}};
        HttpResponse r = PostWithRetry([&] { return s(); }, FastConfig());
        CHECK(r.status == 529);
        CHECK(r.body == "body529");
        CHECK(s.calls == 3);
    }
    {   // 400 is the request's fault: no retry.
        Scripted s{{400, 200}};
        HttpResponse r = PostWithRetry([&] { return s(); }, FastConfig());
        CHECK(r.status == 400);
        CHECK(s.calls == 1);
    }
    {   // Transport error: returned once, not retried.
        Scripted s{{0, 200}};
        HttpResponse r = PostWithRetry([&] { return s(); }, FastConfig());
        CHECK(r.status == 0);
        CHECK(s.calls == 1);
    }

    if (g_failures == 0) std::printf("test_http_retry: OK\n");
    return g_failures == 0 ? 0 : 1;
}
