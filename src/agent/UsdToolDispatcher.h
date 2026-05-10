#pragma once

#include <pxr/base/js/types.h>
#include <pxr/base/js/value.h>
#include <pxr/pxr.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>

#include <functional>
#include <string>

PXR_NAMESPACE_USING_DIRECTIVE

namespace UsdAgent {

// Routes tool calls from the agent to USD C++ functions.
//
// Lifetime: the dispatcher does NOT hold a stage or edit layer at
// construction. Each call resolves them through the providers passed in,
// because the active stage and edit target can change between requests
// (the user switches stages, opens a sublayer, picks a different edit
// target, etc.). In production the providers wrap Editor::GetInstance();
// in tests they return a fixture stage / layer.
//
// Tool semantics:
//   - Inspection tools generally use the composed UsdStageRefPtr.
//   - Edit tools (added in step 7) operate on the current edit-target
//     SdfLayerRefPtr via the existing command system (ExecuteAfterDraw).
//
// Errors: Dispatch never throws. Anything that goes wrong is returned as
// a descriptive string starting with "[error]" so the model can recover.
class UsdToolDispatcher {
public:
    using StageProvider     = std::function<UsdStageRefPtr()>;
    using EditLayerProvider = std::function<SdfLayerRefPtr()>;

    UsdToolDispatcher(StageProvider     stageFn,
                      EditLayerProvider editLayerFn = {});

    std::string Dispatch(const std::string& toolName, const JsObject& args);

    // Per-tool result caps (chosen to keep LLM context bounded).
    static constexpr size_t kFindPrimsLimit       = 50;
    static constexpr int    kListChildrenMaxDepth = 5;

private:
    StageProvider     _stageFn;
    EditLayerProvider _editLayerFn;

    // Inspection tools.
    std::string GetPrimInfo         (const JsObject& args) const;
    std::string GetAttributeValue   (const JsObject& args) const;
    std::string GetValueResolution  (const JsObject& args) const;
    std::string GetCompositionArcs  (const JsObject& args) const;
    std::string GetLayerStack       (const JsObject& args) const;
    std::string ListChildren        (const JsObject& args) const;
    std::string FindPrims           (const JsObject& args) const;

    // Edit tools — queue commands via ExecuteAfterDraw and return immediately.
    // The actual edit lands when the host application drains the command
    // queue (next frame in usdtweak; explicitly via CommandStack::ExecuteCommands
    // in tests). Result strings explain that the edit was queued.
    std::string SetAttribute        (const JsObject& args) const;
    std::string SetActive           (const JsObject& args) const;
    std::string SetVariant          (const JsObject& args) const;
    std::string SetVisibility       (const JsObject& args) const;
};

} // namespace UsdAgent
