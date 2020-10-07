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

//void UndoSetField(SdfLayerHandle layer, const SdfPath& path, const TfToken& fieldName, const VtValue& value) {
//    std::cout << "Set Field " << value << std::endl;
//    if (layer && layer->GetStateDelegate()){
//        layer->GetStateDelegate()->SetField(path, fieldName, value);
//    }
//}
//
//void UndoMoveSpec(SdfLayerHandle layer, const SdfPath& oldPath, const SdfPath& newPath) {
//    std::cout << "Undo move spec " << oldPath.GetString() << " " << newPath.GetString() << std::endl;
//    if (layer && layer->GetStateDelegate()){
//        layer->GetStateDelegate()->MoveSpec(newPath, oldPath);
//    }
//}
//
//void UndoSetFieldDictValueByKey(SdfLayerHandle layer,
//    const SdfPath& path,
//    const TfToken& fieldName,
//    const TfToken& keyPath,
//    const VtValue& previousValue) {
//    std::cout << "Undo set field dict value by key " << std::endl;
//    if (layer && layer->GetStateDelegate()) {
//        layer->GetStateDelegate()->SetFieldDictValueByKey(path, fieldName, keyPath, previousValue);
//    }
//}
//
//void UndoSetTimeSample(SdfLayerHandle layer, const SdfPath& path, double timeCode, const VtValue& previousValue) {
//    std::cout << "UndoSetTimeSample " << std::endl;
//    if (layer && layer->GetStateDelegate()) {
//        layer->GetStateDelegate()->SetTimeSample(path, timeCode, previousValue);
//    }
//}
//
//void UndoCreateSpec(SdfLayerHandle layer, const SdfPath& path, bool inert) {
//    std::cout << "UndoCreateSpec " << std::endl;
//    if (layer && layer->GetStateDelegate()) {
//        layer->GetStateDelegate()->DeleteSpec(path, inert);
//    }
//}
//
//static void _CopySpec(const SdfAbstractData& src, SdfAbstractData* dst,
//                      const SdfPath& path) {
//    dst->CreateSpec(path, src.GetSpecType(path));
//    std::vector<TfToken> fields = src.List(path);
//    TF_FOR_ALL(i, fields) { dst->Set(path, *i, src.Get(path, *i)); }
//}
//
//static void _CopySpecAtPath(const SdfAbstractData& src, SdfAbstractData* dst,
//                            const SdfPath& path) {
//    _CopySpec(src, dst, path);
//}
//
//struct _SpecCopier : public SdfAbstractDataSpecVisitor {
//    explicit _SpecCopier(SdfAbstractData* dst_) : dst(dst_) {}
//
//    bool VisitSpec(const SdfAbstractData& src,
//                   const SdfPath& path) override {
//        _CopySpec(src, dst, path);
//        return true;
//    }
//
//    void Done(const SdfAbstractData&) override {}
//
//    SdfAbstractData* const dst;
//};
//
//void UndoDeleteSpec(SdfLayerHandle layer, const SdfPath& path, bool inert, const SdfSpecType &deletedSpecType, SdfDataRefPtr deletedData, _SpecCopier &specCopier) {
//    std::cout << "UndoDeleteSpec " << std::endl;
//    if (layer && layer->GetStateDelegate()) {
//        SdfChangeBlock changeBlock;
//        layer->GetStateDelegate()->CreateSpec(path, deletedSpecType, inert);
//        deletedData->VisitSpecs(&specCopier);
//    }
//}
//
//void UndoPushChildToken(SdfLayerHandle layer, const SdfPath& parentPath,
//    const TfToken& fieldName,
//    const TfToken& value) {
//    std::cout << "UndoPushChildToken " << std::endl;
//    if (layer && layer->GetStateDelegate()) {
//        layer->GetStateDelegate()->PopChild(parentPath, fieldName, value);
//    }
//}
//
//void UndoPushChildPath(SdfLayerHandle layer, const SdfPath& parentPath,
//    const TfToken& fieldName,
//    const SdfPath& value) {
//    std::cout << "UndoPushChildPath " << std::endl;
//    if (layer && layer->GetStateDelegate()) {
//        layer->GetStateDelegate()->PopChild(parentPath, fieldName, value);
//    }
//}
//
//void UndoPopChildToken(SdfLayerHandle layer, const SdfPath& parentPath,
//    const TfToken& fieldName,
//    const TfToken& previousValue) {
//    std::cout << "UndoPopChildToken " << std::endl;
//    if (layer && layer->GetStateDelegate()) {
//        layer->GetStateDelegate()->PushChild(parentPath, fieldName, previousValue);
//    }
//}
//
//void UndoPopChildPath(SdfLayerHandle layer, const SdfPath& parentPath,
//    const TfToken& fieldName,
//    const SdfPath& previousValue) {
//    std::cout << "UndoPopChildPath " << std::endl;
//    if (layer && layer->GetStateDelegate()) {
//        layer->GetStateDelegate()->PushChild(parentPath, fieldName, previousValue);
//    }
//}

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
    //_undoCommands.AddFunction(std::bind(&UndoSetField, _layer, path, fieldName, previousValue));
    //_redoCommands.AddFunction(std::bind(&UndoSetField, _layer, path, fieldName, newValue));

   // UndoRedoSetField setFieldRecord(_layer, path, fieldName, newValue, previousValue);
    //_undoCommands.AddInstruction<UndoRedoSetField>(std::move(setFieldRecord));

    std::cout << "Storing AddInstruction for redo: " << newValue << " " << previousValue  << std::endl;
    _undoCommands.StoreInstruction<UndoRedoSetField>({_layer, path, fieldName, newValue, previousValue});

    std::cout << "_OnSetField"
        << " " << path.GetString()
        << " " << fieldName.GetString()
        << " " << value
        << std::endl;
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

    /// will that MEMORY LEAK ????? check !
    //_undoCommands.AddFunction(std::bind(&UndoSetField, _layer, path, fieldName, previousValue));
    /// How does that work ?? it shouldn't, VtValue should be deallocated
    //std::cout << "Storing vtvalue for redo: " << vtValue << std::endl;
    //_redoCommands.AddFunction(std::bind(&UndoSetField, _layer, path, fieldName, vtValue));


   // _undoCommands.AddSetFieldCommand(_layer, path, fieldName, previousValue, newValue);
    _undoCommands.StoreInstruction<UndoRedoSetField>({_layer, path, fieldName, newValue, previousValue});
    ///
    VtValue val;
    value.GetValue(&val);
    std::cout << "_OnSetFieldSdfAbstract"
        << " " << path.GetString()
        << " " << fieldName.GetString()
        << " " << val
        << std::endl;
}

void
UndoRedoLayerStateDelegate::_OnSetFieldDictValueByKey(
    const SdfPath& path,
    const TfToken& fieldName,
    const TfToken& keyPath,
    const VtValue& value)
{
    SetDirty();
    const VtValue previousValue = _layer->GetFieldDictValueByKey(path, fieldName, keyPath);
    //_undoCommands.AddFunction(std::bind(&UndoSetFieldDictValueByKey, _layer, path, fieldName, keyPath, previousValue));

    const VtValue newValue = value;

    _undoCommands.StoreInstruction<UndoRedoSetFieldDictValueByKey>({_layer, path, fieldName, keyPath, newValue, previousValue});

    std::cout << "_OnSetFieldDictValueByKey"
        << " " << path.GetString()
        << " " << fieldName.GetString()
        << " " << keyPath.GetString()
        << " " << value
        << std::endl;
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
    //_undoCommands.AddFunction(std::bind(&UndoSetFieldDictValueByKey, _layer, path, fieldName, keyPath, previousValue));

    VtValue newValue;
    value.GetValue(&newValue);
    _undoCommands.StoreInstruction<UndoRedoSetFieldDictValueByKey>({ _layer, path, fieldName, keyPath, newValue, previousValue });

    std::cout << "_OnSetFieldDictValueByKey"
        << " " << path.GetString()
        << " " << fieldName.GetString()
        << " " << keyPath.GetString()
        << " " << newValue
        << std::endl;
}

void
UndoRedoLayerStateDelegate::_OnSetTimeSample(
    const SdfPath& path,
    double timeCode,
    const VtValue& value)
{
    SetDirty();

    _undoCommands.StoreInstruction<UndoRedoSetTimeSample>({_layer, path, timeCode, value });

    //if (!_layer->HasField(path, SdfFieldKeys->TimeSamples)) {
    //     _undoCommands.AddFunction(std::bind(&UndoSetField, _layer, path, SdfFieldKeys->TimeSamples, VtValue()));
    //} else {
    //    VtValue previousValue;
    //    _layer->QueryTimeSample(path, timeCode, &previousValue);
    //    _undoCommands.AddFunction(std::bind(&UndoSetTimeSample, _layer, path, timeCode, previousValue));
    //}

    std::cout << "_OnSetTimeSample"
        << " " << path.GetString()
        << " " << timeCode
        << " " << value
        << std::endl;
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
    //if (!_layer->HasField(path, SdfFieldKeys->TimeSamples)) {
    //     _undoCommands.AddFunction(std::bind(&UndoSetField, _layer, path, SdfFieldKeys->TimeSamples, VtValue()));
    //} else {
    //    VtValue previousValue;
    //    _layer->QueryTimeSample(path, timeCode, &previousValue);
    //    _undoCommands.AddFunction(std::bind(&UndoSetTimeSample, _layer, path, timeCode, previousValue));
    //}

    std::cout << "_OnSetTimeSample"
        << " " << path.GetString()
        << " " << timeCode
        << " " << newValue
        << std::endl;
}

void
UndoRedoLayerStateDelegate::_OnCreateSpec(
    const SdfPath& path,
    SdfSpecType specType,
    bool inert)
{
    SetDirty();
    //_undoCommands.AddFunction(std::bind(&UndoCreateSpec, _layer, path, inert));
    _undoCommands.StoreInstruction<UndoRedoCreateSpec>({_layer, path, specType, inert});

    std::cout << "_OnCreateSpec"
        << " " << path.GetString()
        << " " << specType
        << " " << inert << std::endl;
}

void
UndoRedoLayerStateDelegate::_OnDeleteSpec(
    const SdfPath& path,
    bool inert)
{
    SetDirty();

    _undoCommands.StoreInstruction<UndoRedoDeleteSpec>({_layer, path,  inert, _GetLayerData()});

    //SdfChangeBlock changeBlock;

    //// TODO: is there a faster way of copying and restoring the data ?
    //// This can be really slow on big scenes
    //SdfDataRefPtr deletedData = TfCreateRefPtr(new SdfData());
    //SdfLayer::TraversalFunction copyFunc = std::bind(
    //    &_CopySpecAtPath, boost::cref(*boost::get_pointer(_GetLayerData())),
    //    boost::get_pointer(deletedData), std::placeholders::_1);
    //_layer->Traverse(path, copyFunc);

    //const SdfSpecType deletedSpecType = _layer->GetSpecType(path);
    //_SpecCopier copier(boost::get_pointer(_GetLayerData()));
    //_undoCommands.AddFunction(std::bind(&UndoDeleteSpec, _layer, path, inert, deletedSpecType, deletedData, copier));

    std::cout << "_OnDeleteSpec"
        << " " << path.GetString()
        << " " << inert << std::endl;
}

void
UndoRedoLayerStateDelegate::_OnMoveSpec(
    const SdfPath& oldPath,
    const SdfPath& newPath)
{
    SetDirty();
    //_undoCommands.AddFunction(std::bind(&UndoMoveSpec, _layer, oldPath, newPath));

    _undoCommands.StoreInstruction<UndoRedoMoveSpec>({_layer, oldPath, newPath});

    std::cout << "_OnMoveSpec"
        << " " << oldPath.GetString()
        << " " << newPath.GetString()
        << std::endl;
}

void
UndoRedoLayerStateDelegate::_OnPushChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const TfToken& value)
{
    SetDirty();

    _undoCommands.StoreInstruction<UndoRedoPushChild<TfToken>>({_layer, parentPath, fieldName, value});

    //_undoCommands.AddFunction(std::bind(&UndoPushChildToken, _layer, parentPath, fieldName, value));
    std::cout << "_OnPushChild"
        << " " << parentPath.GetString()
        << " " << fieldName.GetString()
        << " " << value.GetString()
        << std::endl;
}

void
UndoRedoLayerStateDelegate::_OnPushChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const SdfPath& value)
{
    SetDirty();
    //_undoCommands.AddFunction(std::bind(&UndoPushChildPath, _layer, parentPath, fieldName, value));

    _undoCommands.StoreInstruction<UndoRedoPushChild<SdfPath>>({_layer, parentPath, fieldName, value});


    std::cout << "_OnPushChild"
        << " " << parentPath.GetString()
        << " " << fieldName.GetString()
        << " " << value.GetString() << std::endl;
}

void
UndoRedoLayerStateDelegate::_OnPopChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const TfToken& oldValue)
{
    SetDirty();
    _undoCommands.StoreInstruction<UndoRedoPopChild<TfToken>>({_layer, parentPath, fieldName, oldValue});
    //_undoCommands.AddFunction(std::bind(&UndoPopChildToken, _layer, parentPath, fieldName, oldValue));
    std::cout << "_OnPopChild"
        << " " << parentPath.GetString()
        << " " << fieldName.GetString()
        << " " << oldValue.GetString() << std::endl;
}

void
UndoRedoLayerStateDelegate::_OnPopChild(
    const SdfPath& parentPath,
    const TfToken& fieldName,
    const SdfPath& oldValue)
{
    SetDirty();
    _undoCommands.StoreInstruction<UndoRedoPopChild<SdfPath>>({_layer, parentPath, fieldName, oldValue});
    //_undoCommands.AddFunction(std::bind(&UndoPopChildPath, _layer, parentPath, fieldName, oldValue));
    std::cout << "_OnPopChild"
        << " " << parentPath.GetString()
        << " " << fieldName.GetString()
        << " " << oldValue.GetString() << std::endl;
}




