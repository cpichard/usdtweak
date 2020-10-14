#include "UndoLayerStateDelegate.h"
#include "SdfUndoRecorder.h"
#include "SdfLayerInstructions.h"
#include <iostream>

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/abstractData.h>

///
/// UndoRedoLayerStateDelegate is a delegate used to record Undo functions.
/// It's a rip of  https://github.com/LumaPictures/usd-qt/blob/master/pxr/usdQt/undoStateDelegate.h
///

// Original info:
//Brett Levin
//5 Oct 2016, 22:40 : 07
//    to usd - interest
//    At the risk of getting into way too much irrelevant detail, let me try to sketch more of how you could use the delegate to hook into an undo system, since I realize this hasn't been spelled out anywhere.
//    The first thing to say is that while your app is probably working in terms of a UsdStage the actual data edits happen to all the SdfLayers that back it.So undo / redo is really about tracking changes to that set of layers.
//    To support that, each layer owns a delegate, which it will route all edits through.Anything you can modify on a layer through the public API will end up going through one of the delegate methods.This includes anything you do at the Usd API level, which turns around and talks to the Sdf layer backing the UsdStage.
//    That's what all of the public Set(), Create(), and Move() methods on SdfLayerStateDelegateBase are about:  they are the interface the layer talks to its delegate through.  (If you squint, this is just a key/value API like you would see in a key/value store like Redis, but with a slightly more elaborate structure of the data.)
//    Those methods will, in turn, call all of the _On*() private methods on the delegate, and then turn around and make the actual change to the SdfLayer data(via the _PrimSet* methods).Those _On() methods are the hook you can use to track any edits for undo, because they happen before the change has been made, and therefore give you a chance to record the old vs. new values of whatever is changing.
//    Recording that would probably look like binding a function call(ex: C++11 lambda or boost::bind) to invoke the public Set() method again but with suitable arguments to restore the old value.That function call would go onto your undo stack or state graph edge or wherever you are keeping track of edits for undo.As a result, you'll automatically get change notification (and UsdStage refreshes) when undoing operations, and you'll also automatically get redo edits registered when you undo.
//    The other key thing is that you'd need to install a suitable delegate on every layer that gets loaded.  A starting point could be UsdStage::_GetPcpCache()->GetUsedLayers(), which will give the full set of layers that the Usd runtime has pulled in.  I don't know if Usd has a good hook for discovering layers as they are loaded.
//    Ok, that's probably more than you wanted to know but hopefully some of this is helpful,
//    - B
//
// also check https://github.com/LumaPictures/usd-qt/blob/master/pxr/usdQt/undoStateDelegate.h
//

// TODO: We must have Unit tests for this part of the software !

void UndoMarkStateAsDirty(SdfLayerHandle layer) {
    if (layer && layer->GetStateDelegate()) {
        // TODO: investigate why the following code does not reset the dirty flag
        SdfLayerStateDelegateBaseRefPtr previousDelegate = layer->GetStateDelegate();
        SdfCommandGroup dummyCommands;
        auto stateDelegate = UndoRedoLayerStateDelegate::New(dummyCommands);
        layer->SetStateDelegate(stateDelegate);
        stateDelegate->SetClean();
        layer->SetStateDelegate(previousDelegate);
    }
}

UndoRedoLayerStateDelegateRefPtr UndoRedoLayerStateDelegate::New(SdfCommandGroup &instructions)
{
    return TfCreateRefPtr(new UndoRedoLayerStateDelegate(instructions));
}

void UndoRedoLayerStateDelegate::SetClean() {
    _dirty = false;
}

void UndoRedoLayerStateDelegate::SetDirty() {
    if (!_dirty) {
       // TODO _undoCommands.AddFunction(std::bind(&UndoMarkStateAsDirty, _layer));
    }
    _MarkCurrentStateAsDirty();
}

bool
UndoRedoLayerStateDelegate::_IsDirty()
{
    return _dirty;
}

void
UndoRedoLayerStateDelegate::_MarkCurrentStateAsClean()
{
    _dirty = false;
}

void
UndoRedoLayerStateDelegate::_MarkCurrentStateAsDirty()
{
    _dirty = true;
}

void
UndoRedoLayerStateDelegate::_OnSetLayer(
    const SdfLayerHandle& layer)
{
    _layer = layer;
}

void
UndoRedoLayerStateDelegate::_OnSetField(
    const SdfPath& path,
    const TfToken& fieldName,
    const VtValue& value)
{
    SetDirty();
    const VtValue previousValue = _layer->GetField(path, fieldName);
    const VtValue newValue = value;
    _undoCommands.StoreInstruction<UndoRedoSetField>({_layer, path, fieldName, newValue, previousValue});
}

void
UndoRedoLayerStateDelegate::_OnSetField(
    const SdfPath& path,
    const TfToken& fieldName,
    const SdfAbstractDataConstValue& value)
{
    SetDirty();
    const VtValue previousValue = _layer->GetField(path, fieldName);
    VtValue newValue;
    value.GetValue(&newValue);
    _undoCommands.StoreInstruction<UndoRedoSetField>({_layer, path, fieldName, newValue, previousValue});
}

void
UndoRedoLayerStateDelegate::_OnSetFieldDictValueByKey(
    const SdfPath& path,
    const TfToken& fieldName,
    const TfToken& keyPath,
    const VtValue& value)
{
    SetDirty();
    const VtValue previousValue = _layer->GetFieldDictValueByKey(path, fieldName, keyPath); // TODO should the instruction retrieve the value instead ?
    const VtValue newValue = value;
    _undoCommands.StoreInstruction<UndoRedoSetFieldDictValueByKey>({_layer, path, fieldName, keyPath, newValue, previousValue});
}

void
UndoRedoLayerStateDelegate::_OnSetFieldDictValueByKey(
    const SdfPath& path,
    const TfToken& fieldName,
    const TfToken& keyPath,
    const SdfAbstractDataConstValue& value)
{
    SetDirty();
    const VtValue previousValue = _layer->GetFieldDictValueByKey(path, fieldName, keyPath);

    VtValue newValue;
    value.GetValue(&newValue);
    _undoCommands.StoreInstruction<UndoRedoSetFieldDictValueByKey>({_layer, path, fieldName, keyPath, newValue, previousValue});
}

void
UndoRedoLayerStateDelegate::_OnSetTimeSample(
    const SdfPath& path,
    double timeCode,
    const VtValue& value)
{
    SetDirty();
    _undoCommands.StoreInstruction<UndoRedoSetTimeSample>({_layer, path, timeCode, value});
}

void
UndoRedoLayerStateDelegate::_OnSetTimeSample(
    const SdfPath& path,
    double timeCode,
    const SdfAbstractDataConstValue& value)
{
    SetDirty();
    VtValue newValue;
    value.GetValue(&newValue);

    _undoCommands.StoreInstruction<UndoRedoSetTimeSample>({_layer, path, timeCode, newValue});
}

void
UndoRedoLayerStateDelegate::_OnCreateSpec(
    const SdfPath& path,
    SdfSpecType specType,
    bool inert)
{
    SetDirty();
    _undoCommands.StoreInstruction<UndoRedoCreateSpec>({_layer, path, specType, inert});
}

void
UndoRedoLayerStateDelegate::_OnDeleteSpec(
    const SdfPath& path,
    bool inert)
{
    SetDirty();

    _undoCommands.StoreInstruction<UndoRedoDeleteSpec>({_layer, path,  inert, _GetLayerData()});

}

void
UndoRedoLayerStateDelegate::_OnMoveSpec(
    const SdfPath& oldPath,
    const SdfPath& newPath)
{
    SetDirty();
    _undoCommands.StoreInstruction<UndoRedoMoveSpec>({_layer, oldPath, newPath});
}

void
UndoRedoLayerStateDelegate::_OnPushChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const TfToken& value)
{
    SetDirty();
    _undoCommands.StoreInstruction<UndoRedoPushChild<TfToken>>({_layer, parentPath, fieldName, value});
}

void
UndoRedoLayerStateDelegate::_OnPushChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const SdfPath& value)
{
    SetDirty();
    _undoCommands.StoreInstruction<UndoRedoPushChild<SdfPath>>({_layer, parentPath, fieldName, value});
}

void
UndoRedoLayerStateDelegate::_OnPopChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const TfToken& oldValue)
{
    SetDirty();
    _undoCommands.StoreInstruction<UndoRedoPopChild<TfToken>>({_layer, parentPath, fieldName, oldValue});
}

void
UndoRedoLayerStateDelegate::_OnPopChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const SdfPath& oldValue)
{
    SetDirty();
    _undoCommands.StoreInstruction<UndoRedoPopChild<SdfPath>>({_layer, parentPath, fieldName, oldValue});
}




