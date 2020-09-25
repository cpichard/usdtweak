#include "CommandLayerStateDelegate.h"
#include <iostream>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/abstractData.h>

CommandLayerStateDelegateRefPtr
CommandLayerStateDelegate::New()
{
    return TfCreateRefPtr(new CommandLayerStateDelegate);
}

CommandLayerStateDelegate::CommandLayerStateDelegate()
    : _dirty(false)
{
}

bool
CommandLayerStateDelegate::_IsDirty()
{
    return _dirty;
}

void
CommandLayerStateDelegate::_MarkCurrentStateAsClean()
{
    _dirty = false;
}

void
CommandLayerStateDelegate::_MarkCurrentStateAsDirty()
{
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnSetLayer(
    const SdfLayerHandle& layer)
{
}

void
CommandLayerStateDelegate::_OnSetField(
    const SdfPath& path,
    const TfToken& fieldName,
    const VtValue& value)
{
    std::cout << "_OnSetField"
        << " " << path.GetString()
        << " " << fieldName.GetString()
        << " " << value
        << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnSetField(
    const SdfPath& path,
    const TfToken& fieldName,
    const SdfAbstractDataConstValue& value)
{
    VtValue val;
    value.GetValue(&val);
    std::cout << "_OnSetField"
        << " " << path.GetString()
        << " " << fieldName.GetString()
        << " " << val
        << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnSetFieldDictValueByKey(
    const SdfPath& path,
    const TfToken& fieldName,
    const TfToken& keyPath,
    const VtValue& value)
{
    std::cout << "_OnSetFieldDictValueByKey"
        << " " << path.GetString()
        << " " << fieldName.GetString()
        << " " << keyPath.GetString()
        << " " << value
        << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnSetFieldDictValueByKey(
    const SdfPath& path,
    const TfToken& fieldName,
    const TfToken& keyPath,
    const SdfAbstractDataConstValue& value)
{
    VtValue val;
    value.GetValue(&val);
    std::cout << "_OnSetFieldDictValueByKey"
        << " " << path.GetString()
        << " " << fieldName.GetString()
        << " " << keyPath.GetString()
        << " " << val
        << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnSetTimeSample(
    const SdfPath& path,
    double time,
    const VtValue& value)
{
    std::cout << "_OnSetTimeSample"
        << " " << path.GetString()
        << " " << time
        << " " << value
        << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnSetTimeSample(
    const SdfPath& path,
    double time,
    const SdfAbstractDataConstValue& value)
{
    VtValue val;
    value.GetValue(&val);
    std::cout << "_OnSetTimeSample"
        << " "
        << path.GetString()
        << " " << time
        << " " << val
        << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnCreateSpec(
    const SdfPath& path,
    SdfSpecType specType,
    bool inert)
{
    std::cout << "_OnCreateSpec"
        << " " << path.GetString()
        << " " << specType
        << " " << inert << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnDeleteSpec(
    const SdfPath& path,
    bool inert)
{
    std::cout << "_OnDeleteSpec"
        << " " << path.GetString()
        << " " << inert << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnMoveSpec(
    const SdfPath& oldPath,
    const SdfPath& newPath)
{
    std::cout << "_OnMoveSpec"
        << " " << oldPath.GetString()
        << " " << newPath.GetString()
        << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnPushChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const TfToken& value)
{
    std::cout << "_OnPushChild" << " " << parentPath.GetString() << " " << fieldName.GetString() << " " << value.GetString() << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnPushChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const SdfPath& value)
{
    std::cout << "_OnPushChild" << " " << parentPath.GetString() << " " << fieldName.GetString() << " " << value.GetString() << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnPopChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const TfToken& oldValue)
{
    std::cout << "_OnPopChild" << " " << parentPath.GetString() << " " << fieldName.GetString() << " " << oldValue.GetString() << std::endl;
    _dirty = true;
}

void
CommandLayerStateDelegate::_OnPopChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const SdfPath& oldValue)
{
    std::cout << "_OnPopChild" << " " << parentPath.GetString() << " " << fieldName.GetString() << " " << oldValue.GetString() << std::endl;
    _dirty = true;
}
