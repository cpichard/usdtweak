#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usdGeom/gprim.h>

PXR_NAMESPACE_USING_DIRECTIVE

struct AttributeSet : public SdfLayerCommand {

    AttributeSet(UsdStageWeakPtr stage, SdfPath attributePath, VtValue value, UsdTimeCode currentTime)
        : _stage(stage), _attributePath(std::move(attributePath)), _value(std::move(value)), _timeCode(currentTime)
    {}

    ~AttributeSet() override {}

    bool DoIt() override {
        if (_stage){
            auto layer = _stage->GetEditTarget().GetLayer();
            if (layer) {
                SdfUndoRecorder recorder(_undoCommands, layer);
                const UsdAttribute &attribute = _stage->GetAttributeAtPath(_attributePath);
                attribute.Set(_value, _timeCode);
                return true;
            }
        }
        return false;
    }

    UsdStageWeakPtr _stage;
    SdfPath _attributePath;
    VtValue _value;
    UsdTimeCode _timeCode;
};
template void DispatchCommand<AttributeSet>(UsdStageWeakPtr stage, SdfPath attributePath, VtValue value, UsdTimeCode currentTime);
