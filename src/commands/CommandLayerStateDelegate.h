#pragma once

#include <pxr/usd/sdf/layerStateDelegate.h>

PXR_NAMESPACE_USING_DIRECTIVE

TF_DECLARE_WEAK_AND_REF_PTRS(CommandLayerStateDelegate);

class CommandLayerStateDelegate : public SdfLayerStateDelegateBase {
public:
    static CommandLayerStateDelegateRefPtr New();

protected:
    CommandLayerStateDelegate();

    // SdfLayerStateDelegateBase overrides
    virtual bool _IsDirty() override;
    virtual void _MarkCurrentStateAsClean() override;
    virtual void _MarkCurrentStateAsDirty() override;

    virtual void _OnSetLayer(
        const SdfLayerHandle& layer) override;

    virtual void _OnSetField(
        const SdfPath& path,
        const TfToken& fieldName,
        const VtValue& value) override;
    virtual void _OnSetField(
        const SdfPath& path,
        const TfToken& fieldName,
        const SdfAbstractDataConstValue& value) override;
    virtual void _OnSetFieldDictValueByKey(
        const SdfPath& path,
        const TfToken& fieldName,
        const TfToken& keyPath,
        const VtValue& value) override;
    virtual void _OnSetFieldDictValueByKey(
        const SdfPath& path,
        const TfToken& fieldName,
        const TfToken& keyPath,
        const SdfAbstractDataConstValue& value) override;

    virtual void _OnSetTimeSample(
        const SdfPath& path,
        double time,
        const VtValue& value) override;
    virtual void _OnSetTimeSample(
        const SdfPath& path,
        double time,
        const SdfAbstractDataConstValue& value) override;

    virtual void _OnCreateSpec(
        const SdfPath& path,
        SdfSpecType specType,
        bool inert) override;

    virtual void _OnDeleteSpec(
        const SdfPath& path,
        bool inert) override;

    virtual void _OnMoveSpec(
        const SdfPath& oldPath,
        const SdfPath& newPath) override;

    virtual void _OnPushChild(
        const SdfPath& path,
        const TfToken& fieldName,
        const TfToken& value) override;
    virtual void _OnPushChild(
        const SdfPath& path,
        const TfToken& fieldName,
        const SdfPath& value) override;
    virtual void _OnPopChild(
        const SdfPath& path,
        const TfToken& fieldName,
        const TfToken& oldValue) override;
    virtual void _OnPopChild(
        const SdfPath& path,
        const TfToken& fieldName,
        const SdfPath& oldValue) override;

private:
    bool _dirty;



};

