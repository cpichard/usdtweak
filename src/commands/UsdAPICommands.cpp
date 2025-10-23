
#include "Commands.h"
#include "CommandsImpl.h"
#include "SdfUndoRedoRecorder.h"
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

// Command to bind material to a prim
struct UsdAPIMaterialBind : public SdfLayerCommand {

    UsdAPIMaterialBind(UsdPrim prim, SdfPath materialPath, TfToken purpose) : _materialPath(materialPath), _purpose(purpose) {
        if (prim) {
            _stage = prim.GetStage();
            if (_stage) {
                _layer = _stage->GetEditTarget().GetLayer();
                _primPath = prim.GetPath();
            }
        }
    }

    ~UsdAPIMaterialBind() override {}

    bool DoIt() override {
        if (!_layer)
            return false;
        if (_layer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _layer);
            if (_stage) {
                auto prim = _stage->GetPrimAtPath(_primPath);
                if (prim) {
                    UsdShadeMaterialBindingAPI materialBindingAPI(prim);
                    if (_materialPath == SdfPath()) {
                        materialBindingAPI.UnbindDirectBinding(_purpose);
                    } else {
                        UsdShadeMaterial bindableMaterial(_stage->GetPrimAtPath(_materialPath));
                        materialBindingAPI.Bind(bindableMaterial, UsdShadeTokens->strongerThanDescendants, _purpose);
                        materialBindingAPI.Apply(prim);
                    }
                }
            }
            return true;
        }
        return false;
    }

    UsdStageRefPtr _stage;
    SdfLayerRefPtr _layer;
    SdfPath _primPath;
    SdfPath _materialPath;
    TfToken _purpose;
};

template void ExecuteAfterDraw<UsdAPIMaterialBind>(UsdPrim prim, SdfPath materialPath, TfToken purpose);

struct PrimApplySchemas : public SdfLayerCommand {

    PrimApplySchemas(UsdPrim prim, std::vector<std::string> newSchemaNames) : _schemaNames(newSchemaNames) {
        if (prim) {
            _stage = prim.GetStage();
            if (_stage) {
                _layer = _stage->GetEditTarget().GetLayer();
                _primPath = prim.GetPath();
            }
        }
    }

    ~PrimApplySchemas() override {}

    bool DoIt() override {
        // Tokenike the string, look for what has been added and what has been removed
        if (!_layer)
            return false;
        if (_layer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _layer);
            if (_stage) {
                auto prim = _stage->GetPrimAtPath(_primPath);
                if (prim) {
                    // Sort the new schemas
                    std::sort(std::begin(_schemaNames), std::end(_schemaNames));
                    // Get the currently applied schemas as vector of string
                    std::vector<std::string> appliedSchemas;
                    for (const auto schemaToken : prim.GetAppliedSchemas()) {
                        appliedSchemas.push_back(schemaToken.GetString());
                    }
                    std::sort(std::begin(appliedSchemas), std::end(appliedSchemas));

                    // Compute schemas to remove
                    std::vector<std::string> difference;
                    std::set_difference(std::begin(appliedSchemas), std::end(appliedSchemas), std::begin(_schemaNames),
                                        std::end(_schemaNames), std::back_inserter(difference));
                    for (const std::string &item : difference) {
                        prim.RemoveAppliedSchema(TfToken(item));
                    }
                    // Schemas to add
                    difference.clear();
                    std::set_difference(std::begin(_schemaNames), std::end(_schemaNames), std::begin(appliedSchemas),
                                        std::end(appliedSchemas), std::back_inserter(difference));

                    for (const std::string &item : difference) {
                        prim.AddAppliedSchema(TfToken(item));
                    }
                }
            }
            return true;
        }
        return false;
    }

    std::vector<std::string> _schemaNames;
    SdfLayerRefPtr _layer; // Target layer at the time of the command creation
    UsdStageRefPtr _stage;
    SdfPath _primPath; // usd prim path at the time of the creation
};

template void ExecuteAfterDraw<PrimApplySchemas>(UsdPrim prim, std::vector<std::string> appliedSchemas);
