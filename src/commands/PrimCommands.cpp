///
/// This file should be included in Commands.cpp to be compiled with it.
/// IT SHOULD NOT BE COMPILED SEPARATELY
///

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>

PXR_NAMESPACE_USING_DIRECTIVE

struct PrimNew : public SdfLayerCommand {

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
            SdfUndoRecorder recorder(_undoCommands, _layer);
            _newPrimSpec = SdfPrimSpec::New(_layer, _primName, SdfSpecifier::SdfSpecifierDef);
            _layer->InsertRootPrim(_newPrimSpec);
            return true;
        } else {
            SdfUndoRecorder recorder(_undoCommands, _primSpec->GetLayer());
            _newPrimSpec = SdfPrimSpec::New(_primSpec, _primName, SdfSpecifier::SdfSpecifierDef);
            return true;
        }
    }

    SdfPrimSpecHandle _newPrimSpec;
    SdfPrimSpecHandle _primSpec;
    SdfLayerRefPtr _layer;
    std::string _primName;
};

struct PrimRemove : public SdfLayerCommand {

    PrimRemove(SdfLayerRefPtr layer, SdfPrimSpecHandle primSpec)
        :  _primSpec(std::move(primSpec)), _layer(std::move(layer)) {}

    ~PrimRemove() override {}

    bool DoIt() override {
        if (!_primSpec)
            return false;
        SdfUndoRecorder recorder(_undoCommands, _layer);
        if (_primSpec->GetNameParent()) {
            _primSpec->GetNameParent()->RemoveNameChild(_primSpec);
            return true;
        } else {
            _layer->RemoveRootPrim(_primSpec);
            return true;
        }
    }

    SdfLayerRefPtr _layer;
    SdfPrimSpecHandle _primSpec;
};

struct PrimChangeName : public SdfLayerCommand {

    PrimChangeName(SdfPrimSpecHandle primSpec, std::string newName)
        : _primSpec(std::move(primSpec)), _newName(std::move(newName)) {}

    ~PrimChangeName() override {}

    bool DoIt() override {
        SdfUndoRecorder recorder(_undoCommands, _primSpec->GetLayer());
        _primSpec->SetName(_newName);
        return true;
    }

    SdfPrimSpecHandle _primSpec;
    std::string _newName;
};

struct PrimChangeSpecifier : public SdfLayerCommand {
    PrimChangeSpecifier(SdfPrimSpecHandle primSpec, SdfSpecifier newSpecifier)
        : _primSpec(std::move(primSpec)), _newSpecifier(std::move(newSpecifier)) {}

    ~PrimChangeSpecifier() override {}

    bool DoIt() override {
        if (_primSpec) {
            SdfUndoRecorder recorder(_undoCommands, _primSpec->GetLayer());
            _primSpec->SetSpecifier(_newSpecifier);
            return true;
        }
        return false;
    }

    SdfPrimSpecHandle _primSpec;
    SdfSpecifier _newSpecifier;
};

struct PrimAddReference : public SdfLayerCommand {
    PrimAddReference(SdfPrimSpecHandle primSpec, std::string reference)
        : _primSpec(std::move(primSpec)), _reference(std::move(reference)) {}

    ~PrimAddReference() override {}

    bool DoIt() override {
        if (_primSpec) {
            SdfUndoRecorder recorder(_undoCommands, _primSpec->GetLayer());
            auto references = _primSpec->GetReferenceList();
            references.Add(SdfReference(_reference));
            return true;
        }
        return false;
    }

    SdfPrimSpecHandle _primSpec;
    std::string _reference;
};


/// TODO: how to avoid having to write the argument list ? it's the same as the constructor arguments
template void ExecuteAfterDraw<PrimNew>(SdfLayerRefPtr layer, std::string newName);
template void ExecuteAfterDraw<PrimNew>(SdfPrimSpecHandle primSpec, std::string newName);
template void ExecuteAfterDraw<PrimRemove>(SdfLayerRefPtr layer, SdfPrimSpecHandle primSpec);
template void ExecuteAfterDraw<PrimChangeName>(SdfPrimSpecHandle primSpec, std::string newName);
template void ExecuteAfterDraw<PrimChangeSpecifier>(SdfPrimSpecHandle primSpec, SdfSpecifier newSpec);
template void ExecuteAfterDraw<PrimAddReference>(SdfPrimSpecHandle primSpec, std::string reference);


