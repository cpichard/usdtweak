
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/sdf/layer.h>
#include "Commands.h"
#include "CommandsImpl.h"
#include "SdfUndoRedoRecorder.h"


// Command to bind material to a prim
struct UsdAPIMaterialBind : public SdfLayerCommand {
    
    UsdAPIMaterialBind(UsdPrim prim, SdfPath materialPath, TfToken purpose)
    : _materialPath(materialPath), _purpose(purpose) {
        if (prim) {
            _stage = prim.GetStage();
            if (_stage) {
                _layer = _stage->GetEditTarget().GetLayer();
                _primPath = prim.GetPath();
            }
        }
    }

    ~UsdAPIMaterialBind () override {}

    bool DoIt() override {
        if (!_layer)
            return false;
        if (_layer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _layer);
            if (_stage) {
                auto prim = _stage->GetPrimAtPath(_primPath);
                if (prim) {
                    UsdShadeMaterialBindingAPI materialBindingAPI(prim);
                    UsdShadeMaterial bindableMaterial(_stage->GetPrimAtPath(_materialPath));
                    materialBindingAPI.Bind(bindableMaterial, UsdShadeTokens->strongerThanDescendants, _purpose);
                    materialBindingAPI.Apply(prim);
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
