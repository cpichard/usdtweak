#pragma once

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <functional>

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Declarations of Command classes only.
/// We want to have different implementations with or without undo/redo
///
struct PrimNew;
struct PrimRemove;
struct PrimChangeName;
struct PrimChangeSpecifier;
struct PrimAddReference;

struct EditorSelectPrimPath;
struct EditorOpenStage;

struct LayerRemoveSubLayer;
struct LayerInsertSubLayer;
struct LayerMoveSubLayer;

struct UndoCommand;
struct RedoCommand;

struct AttributeSet;

struct UsdApiFunction;

/// Post a command to be executed after the editor frame is rendered.
template<typename CommandClass, typename... ArgTypes>
void DispatchCommand(ArgTypes... arguments);

/// Convenience function to defer the execution of a function from the USD api after the editor frame is rendered.
/// It will also record the changes made  on the layer by the function and store a command in the undo/redo.
/// For the widget API ExecuteAfterRender might just be the call to the original function
template<typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, SdfLayerRefPtr stageOrLayer, ArgsT&&... arguments) {
    std::function<void()> usdApiFunc = std::bind(func, stageOrLayer, std::forward<ArgsT>(arguments)...);
    DispatchCommand<UsdApiFunction>(stageOrLayer, usdApiFunc);
}

// TODO: UsdStageRefPtr instead of pointer
template<typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, UsdStage * &&stageOrLayer, ArgsT&&... arguments) {
    std::function<void()> usdApiFunc = std::bind(func, stageOrLayer, std::forward<ArgsT>(arguments)...);
    auto layer = TfCreateRefPtrFromProtectedWeakPtr(stageOrLayer->GetEditTarget().GetLayer());
    DispatchCommand<UsdApiFunction>(layer, usdApiFunc);
}

template<typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, SdfPrimSpecHandle handle, ArgsT&&... arguments) {
    // Some kind of trickery to recover the SdfPrimSpecHandle at the time of the call.
    // We store its path and layer in a lambda function
    const auto &path = handle->GetPath();
    SdfLayerRefPtr layer = handle->GetLayer();
    std::function<void()> usdApiFunc = [=]() {
        auto primSpec = layer->GetPrimAtPath(path);
        std::function<void()> primSpecFunc = std::bind(func, get_pointer(primSpec), arguments...);
        primSpecFunc();
    };
    DispatchCommand<UsdApiFunction>(layer, usdApiFunc);
}

template<typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, const UsdPrim &prim, ArgsT&&... arguments) {
    const auto &path = prim.GetPath();
    UsdStageWeakPtr stage =  prim.GetStage();
    std::function<void()> usdApiFunc = [=]() {
        auto prim = stage->GetPrimAtPath(path);
        std::function<void()> primFunc = std::bind(func, &prim, arguments...);
        primFunc();
    };
    DispatchCommand<UsdApiFunction>(stage->GetEditTarget().GetLayer(), usdApiFunc);
}

template<typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, UsdGeomImageable &geom, ArgsT&&... arguments) {
    const auto &path =  geom.GetPrim().GetPath();
    UsdStageWeakPtr stage =  geom.GetPrim().GetStage();
    std::function<void()> usdApiFunc = [=]() {
        UsdGeomImageable geomPrim = UsdGeomImageable::Get(stage, path);
        std::function<void()> primFunc = std::bind(func, &geomPrim, arguments...);
        primFunc();
    };
    DispatchCommand<UsdApiFunction>(stage->GetEditTarget().GetLayer(), usdApiFunc);
}

template<typename FuncT, typename... ArgsT>
void ExecuteAfterDraw(FuncT &&func, SdfAttributeSpecHandle att, ArgsT&&... arguments) {
    const auto path = att->GetPath();
    const auto layer = att->GetLayer();
    std::function<void()> usdApiFunc = [=]() {
        auto primSpec = layer->GetPrimAtPath(path);
        std::function<void()> attFunc = std::bind(func, get_pointer(att), arguments...);
        attFunc();
    };
    DispatchCommand<UsdApiFunction>(layer, usdApiFunc);
}


/// Process the commands waiting. Only one command would be waiting at the moment
void ProcessCommands();

///
/// Allows to record one command spanning multiple frames.
/// It is used in the manipulators, to record only one command for a translation/rotation etc.
///
void BeginEdition(UsdStageRefPtr);
void BeginEdition(SdfLayerRefPtr);
void EndEdition();
