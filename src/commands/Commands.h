#pragma once

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usd/variantSets.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/xformCommonAPI.h>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Declarations of Command classes only.
/// The implementation should depend on the application
///
struct PrimNew;
struct PrimRemove;
struct PrimCreateCompositionArc;
struct PrimReparent;

struct EditorSetDataPointer;
struct EditorSelectPrimPath;
struct EditorOpenStage;
struct EditorOpenLayer;
struct EditorSetEditTarget;
struct EditorSetCurrentLayer;

struct LayerRemoveSubLayer;
struct LayerMoveSubLayer;

struct UndoCommand;
struct RedoCommand;

struct AttributeSet;
struct AttributeCreateDefaultValue;

struct UsdFunctionCall;

/// Post a command to be executed after the editor frame is rendered.
/// The commands are defined in Commands.cpp and its included file
template <typename CommandClass, typename... ArgTypes> void ExecuteAfterDraw(ArgTypes... arguments);

/// Convenience functions to avoid creating commands and directly call the USD api after the editor frame is rendered.
/// It will also record the changes made on the layer by the function and store a command in the undo/redo.
template <typename FuncT, typename... ArgsT> void ExecuteAfterDraw(FuncT &&func, SdfLayerRefPtr layer, ArgsT &&...arguments) {
    std::function<void()> usdApiFunc = std::bind(func, layer, std::forward<ArgsT>(arguments)...);
    ExecuteAfterDraw<UsdFunctionCall>(layer, usdApiFunc);
}

template <typename FuncT, typename... ArgsT> void ExecuteAfterDraw(FuncT &&func, UsdStageRefPtr stage, ArgsT &&...arguments) {
    std::function<void()> usdApiFunc = std::bind(func, stage, std::forward<ArgsT>(arguments)...);
    auto layer = TfCreateRefPtrFromProtectedWeakPtr(stage->GetEditTarget().GetLayer());
    ExecuteAfterDraw<UsdFunctionCall>(layer, usdApiFunc);
}

template <typename FuncT, typename... ArgsT> void ExecuteAfterDraw(FuncT &&func, SdfPrimSpecHandle handle, ArgsT &&...arguments) {
    // Some kind of trickery to recover the SdfPrimSpecHandle at the time of the call.
    // We store its path and layer in a lambda function
    const auto &path = handle->GetPath();
    SdfLayerRefPtr layer = handle->GetLayer();
    std::function<void()> usdApiFunc = [=]() {
        auto primSpec = layer->GetPrimAtPath(path);
        std::function<void()> primSpecFunc = std::bind(func, get_pointer(primSpec), arguments...);
        primSpecFunc();
    };
    ExecuteAfterDraw<UsdFunctionCall>(layer, usdApiFunc);
}

template <typename FuncT, typename... ArgsT> void ExecuteAfterDraw(FuncT &&func, const UsdPrim &prim, ArgsT &&...arguments) {
    const auto &path = prim.GetPath();
    UsdStageWeakPtr stage = prim.GetStage();
    std::function<void()> usdApiFunc = [=]() {
        auto prim = stage->GetPrimAtPath(path);
        std::function<void()> primFunc = std::bind(func, &prim, arguments...);
        primFunc();
    };
    ExecuteAfterDraw<UsdFunctionCall>(stage->GetEditTarget().GetLayer(), usdApiFunc);
}

template <typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, const UsdAttribute &attribute, ArgsT &&...arguments) {
    const auto &path = attribute.GetPath();
    UsdStageWeakPtr stage = attribute.GetStage();
    std::function<void()> usdApiFunc = [=]() {
        auto attribute = stage->GetAttributeAtPath(path);
        std::function<void()> attributeFunc = std::bind(func, &attribute, arguments...);
        attributeFunc();
    };
    ExecuteAfterDraw<UsdFunctionCall>(stage->GetEditTarget().GetLayer(), usdApiFunc);
}

template <typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, const UsdRelationship &relationship, ArgsT &&...arguments) {
    const auto &path = relationship.GetPath();
    UsdStageWeakPtr stage = relationship.GetStage();
    std::function<void()> usdApiFunc = [=]() {
        auto relationship = stage->GetRelationshipAtPath(path);
        std::function<void()> relationshipFunc = std::bind(func, &relationship, arguments...);
        relationshipFunc();
    };
    ExecuteAfterDraw<UsdFunctionCall>(stage->GetEditTarget().GetLayer(), usdApiFunc);
}

template <typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, const UsdVariantSet &variantSet, ArgsT &&...arguments) {
    const auto &prim = variantSet.GetPrim();
    const auto &variantName = variantSet.GetName();
    const auto &path = prim.GetPath();
    UsdStageWeakPtr stage = prim.GetStage();
    std::function<void()> usdApiFunc = [=]() {
        auto prim = stage->GetPrimAtPath(path);
        if (prim.HasVariantSets()) {
            auto variantSet = prim.GetVariantSet(variantName);
            std::function<void()> variantSetFunc = std::bind(func, &variantSet, arguments...);
            variantSetFunc();
        }
    };
    ExecuteAfterDraw<UsdFunctionCall>(stage->GetEditTarget().GetLayer(), usdApiFunc);
}

template <typename FuncT, typename... ArgsT> void ExecuteAfterDraw(FuncT &&func, UsdGeomImageable &geom, ArgsT &&...arguments) {
    const auto &path = geom.GetPrim().GetPath();
    UsdStageWeakPtr stage = geom.GetPrim().GetStage();
    std::function<void()> usdApiFunc = [=]() {
        UsdGeomImageable geomPrim = UsdGeomImageable::Get(stage, path);
        std::function<void()> primFunc = std::bind(func, &geomPrim, arguments...);
        primFunc();
    };
    ExecuteAfterDraw<UsdFunctionCall>(stage->GetEditTarget().GetLayer(), usdApiFunc);
}

template <typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, SdfAttributeSpecHandle att, ArgsT &&...arguments) {
    const auto path = att->GetPath();
    const auto layer = att->GetLayer();
    std::function<void()> usdApiFunc = [=]() {
        auto primSpec = layer->GetPrimAtPath(path);
        std::function<void()> attFunc = std::bind(func, get_pointer(att), arguments...);
        attFunc();
    };
    ExecuteAfterDraw<UsdFunctionCall>(layer, usdApiFunc);
}

template <typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, const UsdGeomXformCommonAPI &api, ArgsT &&...arguments) {
    const auto prim = api.GetPrim();
    if (prim) {
        const auto path = prim.GetPath();
        const auto stage = prim.GetStage();
        std::function<void()> usdApiFunc = [=]() {
            UsdGeomXformCommonAPI api(stage->GetPrimAtPath(path));
            std::function<void()> apiFunc = std::bind(func, api, arguments...);
            apiFunc();
        };
        ExecuteAfterDraw<UsdFunctionCall>(stage->GetEditTarget().GetLayer(), usdApiFunc);
    }
}

/// Process the commands waiting in the queue. Only one command would be waiting at the moment
void ExecuteCommands();

///
/// Allows to record one command spanning multiple frames.
/// It is used in the manipulators, to record only one command for a translation/rotation etc.
///
void BeginEdition(UsdStageRefPtr);
void BeginEdition(SdfLayerRefPtr);
void EndEdition();
