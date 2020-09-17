///
/// This file should be included in Commands.cpp to be compiled with it.
/// IT SHOULD NOT BE COMPILED SEPARATELY
///

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>

PXR_NAMESPACE_USING_DIRECTIVE

struct PrimNew : public Command {

    // Create a root prim
    PrimNew(SdfLayerRefPtr layer, std::string primName)
        : _primSpec(), _layer(layer), _primName(std::move(primName)) {}

    // Create a child prim
    PrimNew(SdfPrimSpecHandle primSpec, std::string primName)
        : _primSpec(std::move(primSpec)), _layer(), _primName(std::move(primName)) {}

    ~PrimNew() override {}

    bool DoIt() override {
        if (!_layer && !_primSpec)
            return false;
        if (_layer) {
            _newPrimSpec = SdfPrimSpec::New(_layer, _primName, SdfSpecifier::SdfSpecifierDef);
            _layer->InsertRootPrim(_newPrimSpec);
            return true;
        } else {
            _newPrimSpec = SdfPrimSpec::New(_primSpec, _primName, SdfSpecifier::SdfSpecifierDef);
            return true;
        }
    }

    bool UndoIt() override {
        if (!_newPrimSpec)
            return false;

        if (_layer) {
            _layer->RemoveRootPrim(_newPrimSpec);
        } else if (_newPrimSpec->GetNameParent()) {
            _newPrimSpec->GetNameParent()->RemoveNameChild(_newPrimSpec);
        }
        return false;
    }

    SdfPrimSpecHandle _newPrimSpec;
    SdfPrimSpecHandle _primSpec;
    SdfLayerRefPtr _layer;
    std::string _primName;
};

struct PrimRemove : public Command {

    PrimRemove(SdfLayerRefPtr layer, SdfPrimSpecHandle primSpec)
        :  _primSpec(std::move(primSpec)), _layer(std::move(layer)) {}

    ~PrimRemove() override {}

    bool DoIt() override {
        if (!_primSpec)
            return false;
        if (_primSpec->GetNameParent()) {
            _primSpec->GetNameParent()->RemoveNameChild(_primSpec);
            return true;
        } else {
            _layer->RemoveRootPrim(_primSpec);
            return true;
        }
    }

    bool UndoIt() override {
        // TODO: how do we keep the prim in memory outside of the stage ...
        // is it even possible ?
        // or do we have to create multiple commands in a command group to recreate the state ?
        // Use a delegate instead ? https://groups.google.com/g/usd-interest/c/FyTZPEvN_i4/m/TxIG9z6OGwAJ
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

        return false;
    }

    SdfLayerRefPtr _layer;
    SdfPrimSpecHandle _primSpec;
};

struct PrimChangeName : public Command {

    PrimChangeName(SdfPrimSpecHandle primSpec, std::string newName)
        : _primSpec(std::move(primSpec)), _newName(std::move(newName)) {}

    ~PrimChangeName() override {}

    bool DoIt() override {
        _oldName = _primSpec->GetName();
        _primSpec->SetName(_newName);
        return true;
    }

    bool UndoIt() override {
        _primSpec->SetName(_oldName);
        return true;
    }

    SdfPrimSpecHandle _primSpec;
    std::string _newName;
    std::string _oldName;
};

struct PrimChangeSpecifier : public Command {
    PrimChangeSpecifier(SdfPrimSpecHandle primSpec, SdfSpecifier newSpecifier)
        : _primSpec(std::move(primSpec)), _newSpecifier(std::move(newSpecifier)) {}

    ~PrimChangeSpecifier() override {}

    bool DoIt() override {
        if (_primSpec) {
            _primSpec->SetSpecifier(_newSpecifier);
            return true;
        }
        return false;
    }

    SdfPrimSpecHandle _primSpec;
    SdfSpecifier _newSpecifier;
};

struct PrimAddReference : public Command {
    PrimAddReference(SdfPrimSpecHandle primSpec, std::string reference)
        : primSpec(std::move(primSpec)), reference(std::move(reference)) {}

    ~PrimAddReference() override {}

    bool DoIt() override {
        if (primSpec) {
            auto references = primSpec->GetReferenceList();
            references.Add(SdfReference(reference));
            return true;
        }
        return false;
    }

    SdfPrimSpecHandle primSpec;
    std::string reference;
};

/// TODO: how to avoid having to write the argument list ? it's the same as the constructor arguments
template void DispatchCommand<PrimNew>(SdfLayerRefPtr layer, std::string newName);
template void DispatchCommand<PrimNew>(SdfPrimSpecHandle primSpec, std::string newName);
template void DispatchCommand<PrimRemove>(SdfLayerRefPtr layer, SdfPrimSpecHandle primSpec);
template void DispatchCommand<PrimChangeName>(SdfPrimSpecHandle primSpec, std::string newName);
template void DispatchCommand<PrimChangeSpecifier>(SdfPrimSpecHandle primSpec, SdfSpecifier newSpec);
template void DispatchCommand<PrimAddReference>(SdfPrimSpecHandle primSpec, std::string reference);
