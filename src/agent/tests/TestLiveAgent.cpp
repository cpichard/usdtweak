// Step 6 full integration test. Builds an in-memory USD stage with two layers
// (asset + shot) where the shot overrides the asset's visibility to make
// /World/Hero invisible. Drives the AgentOrchestrator end-to-end against the
// real UsdToolDispatcher and asks two questions.
//
// Skipped if ANTHROPIC_API_KEY is not set in the environment (exit 0).
//   ANTHROPIC_API_KEY=sk-ant-... build-26.03/src/agent/test_live_agent

#include "AgentOrchestrator.h"
#include "AnthropicBackend.h"
#include "LLMBackend.h"
#include "UsdToolDispatcher.h"
#include "UsdTools.h"

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE
using namespace UsdAgent;

namespace {

const char* kAssetLayer = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World" (
    kind = "group"
)
{
    def Xform "Hero" (
        kind = "component"
    )
    {
        token visibility = "inherited"
    }

    def Camera "Camera"
    {
        float focalLength = 35.0
    }

    def Xform "Lights" (
        kind = "group"
    )
    {
        token purpose = "render"
    }
}
)";

// Shot layer overrides Hero.visibility to "invisible".
const char* kShotLayer = R"(#usda 1.0
(
    subLayers = [
        @asset.usda@
    ]
)

over "World"
{
    over "Hero"
    {
        token visibility = "invisible"
    }
}
)";

UsdStageRefPtr BuildFixture(SdfLayerRefPtr* outShot) {
    SdfLayerRefPtr asset = SdfLayer::CreateAnonymous("asset.usda");
    asset->ImportFromString(kAssetLayer);

    SdfLayerRefPtr shot  = SdfLayer::CreateAnonymous("shot.usda");
    shot->ImportFromString(kShotLayer);
    shot->SetSubLayerPaths({asset->GetIdentifier()});

    *outShot = shot;
    return UsdStage::Open(shot);
}

std::string SystemPrompt() {
    return
        "You are a USD scene assistant integrated into usdtweak, a USD "
        "file editor. The user is USD-literate; use USD terminology freely "
        "(prim, layer, variant, composition arc, LIVRPS, edit target).\n"
        "RULES:\n"
        "- Always call a tool to get information before answering factual "
        "questions about the scene. Do not guess or invent values.\n"
        "- Make ONE tool call per turn. Do not request parallel tool calls.\n"
        "- Keep answers concise. Cite the prim path and the layer that "
        "introduced the relevant opinion when explaining a value.\n";
}

void RunQuery(AgentOrchestrator& agent,
              const std::string& question) {
    std::fprintf(stdout, "\n========================================\n");
    std::fprintf(stdout, "Q: %s\n", question.c_str());
    std::fprintf(stdout, "----------------------------------------\n");

    auto trace = [](const std::string& line) {
        std::fprintf(stdout, "  %s\n", line.c_str());
    };

    std::string answer = agent.Run(SystemPrompt(), question, /*history*/{}, trace);

    std::fprintf(stdout, "----------------------------------------\nA: %s\n",
                 answer.c_str());
}

} // namespace

int main() {
    const char* keyEnv = std::getenv("ANTHROPIC_API_KEY");
    if (!keyEnv || !*keyEnv) {
        std::fprintf(stdout, "test_live_agent: skipped (ANTHROPIC_API_KEY not set)\n");
        return 0;
    }

    const char* modelEnv = std::getenv("ANTHROPIC_MODEL");
    std::string model = (modelEnv && *modelEnv) ? modelEnv : "claude-sonnet-4-6";

    SdfLayerRefPtr shot;
    UsdStageRefPtr stage = BuildFixture(&shot);
    if (!stage) {
        std::fprintf(stderr, "test_live_agent: failed to build fixture\n");
        return 1;
    }

    UsdToolDispatcher dispatcher(
        /*stageFn*/    [&]() { return stage; },
        /*editLayerFn*/[&]() { return shot;  });

    auto backend = std::make_unique<AnthropicBackend>(keyEnv, model);
    AgentOrchestrator agent(std::move(backend), dispatcher,
                            BuildReadOnlyToolDefinitions());

    std::fprintf(stdout, "test_live_agent: model=%s\n", model.c_str());

    RunQuery(agent, "What prims are in the scene?");
    RunQuery(agent, "Why is /World/Hero invisible?");

    std::fprintf(stdout, "\ntest_live_agent: OK\n");
    return 0;
}
