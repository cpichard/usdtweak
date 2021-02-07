///
/// This file should be included in Commands.cpp to be compiled with it.
/// IT SHOULD NOT BE COMPILED SEPARATELY
///

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/namespaceEdit.h>

PXR_NAMESPACE_USING_DIRECTIVE

struct PrimNew : public SdfLayerCommand {

    // Create a root prim
    PrimNew(SdfLayerRefPtr layer, std::string primName) : _primSpec(), _layer(layer), _primName(std::move(primName)) {}

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

    PrimRemove(SdfPrimSpecHandle primSpec) : _primSpec(std::move(primSpec)) {}

    ~PrimRemove() override {}

    bool DoIt() override {
        if (!_primSpec)
            return false;
        auto layer = _primSpec->GetLayer();
        SdfUndoRecorder recorder(_undoCommands, layer);
        if (_primSpec->GetNameParent()) {
            // Case where the prim is a variant
            // I am not 100% sure this it the way to do it
            if (_primSpec->GetPath().IsPrimVariantSelectionPath()) {
                auto selection = _primSpec->GetPath().GetVariantSelection();
                TF_FOR_ALL(variantSet, _primSpec->GetNameParent()->GetVariantSets()) {
                    if (variantSet->first == selection.first) {
                        SdfVariantSetSpecHandle variantSetSpec = variantSet->second;
                        SdfVariantSpecHandle variantSpec = variantSetSpec->GetVariants().get(selection.second);
                        if (variantSpec) {
                            variantSetSpec->RemoveVariant(variantSpec);
                            return true;
                        }
                    }
                }
                return false;
            } else {
                return _primSpec->GetNameParent()->RemoveNameChild(_primSpec);
            }
        } else {
            layer->RemoveRootPrim(_primSpec);
            return true;
        }
    }

    SdfPrimSpecHandle _primSpec;
};

#include "ProxyHelpers.h"

template <typename ItemType> struct PrimCreateListEditorOperation : SdfLayerCommand {
    PrimCreateListEditorOperation(SdfPrimSpecHandle primSpec, int operation, typename ItemType::value_type item)
        : _primSpec(primSpec), _operation(operation), _item(std::move(item)) {}
    ~PrimCreateListEditorOperation() override {}

    bool DoIt() override {
        if (_primSpec) {
            SdfUndoRecorder recorder(_undoCommands, _primSpec->GetLayer());
            CreateListEditorOperation(GetListEditor(), _operation, _item);
            return true;
        }
        return false;
    }

    // Forced to inherit as the Specialize and Inherit arcs have the same type
    virtual SdfListEditorProxy<ItemType> GetListEditor() = 0;

    SdfPrimSpecHandle _primSpec;
    int _operation;
    typename ItemType::value_type _item;
};

struct PrimCreateReference : public PrimCreateListEditorOperation<SdfReferenceTypePolicy> {
    using PrimCreateListEditorOperation<SdfReferenceTypePolicy>::PrimCreateListEditorOperation;
    SdfReferencesProxy GetListEditor() override { return _primSpec->GetReferenceList(); }
};

struct PrimCreatePayload : public PrimCreateListEditorOperation<SdfPayloadTypePolicy> {
    using PrimCreateListEditorOperation<SdfPayloadTypePolicy>::PrimCreateListEditorOperation;
    SdfPayloadsProxy GetListEditor() override { return _primSpec->GetPayloadList(); }
};

struct PrimCreateInherit : public PrimCreateListEditorOperation<SdfPathKeyPolicy> {
    using PrimCreateListEditorOperation<SdfPathKeyPolicy>::PrimCreateListEditorOperation;
    SdfInheritsProxy GetListEditor() override { return _primSpec->GetInheritPathList(); }
};

struct PrimCreateSpecialize : public PrimCreateListEditorOperation<SdfPathKeyPolicy> {
    using PrimCreateListEditorOperation<SdfPathKeyPolicy>::PrimCreateListEditorOperation;
    SdfSpecializesProxy GetListEditor() override { return _primSpec->GetSpecializesList(); }
};


// TODO : REMOVE REFERENCE EDITOR
#include "ReferenceEditor.h"

struct PrimCreateCompositionArc : public SdfLayerCommand {
    PrimCreateCompositionArc(SdfPrimSpecHandle primSpec, CompositionOperation operation, CompositionList composition,
                             std::string referencePath, SdfPath targetPrimPath)
        : _primSpec(std::move(primSpec)), _referencePath(std::move(referencePath)), _operation(operation),
          _composition(composition), _targetPrimPath(targetPrimPath) {}

    ~PrimCreateCompositionArc() override {}

    bool DoIt() override {
        if (_primSpec) {
            SdfUndoRecorder recorder(_undoCommands, _primSpec->GetLayer());
            ApplyOperationOnCompositionList(_primSpec, _operation, _composition, _referencePath, _targetPrimPath);
            return true;
        }
        return false;
    }

    CompositionOperation _operation;
    CompositionList _composition;
    SdfPrimSpecHandle _primSpec;
    std::string _referencePath;
    SdfPath _targetPrimPath;
};

struct PrimReparent : public SdfLayerCommand {
    PrimReparent(SdfLayerHandle layer, SdfPath source, SdfPath destination)
        : _layer(std::move(layer)), _source(source), _destination(destination) {}

    ~PrimReparent() override {}

    bool DoIt() override {
        if (!_layer)
            return false;
        SdfUndoRecorder recorder(_undoCommands, _layer);
        SdfNamespaceEdit reparentEdit = SdfNamespaceEdit::Reparent(_source, _destination, 0);
        SdfBatchNamespaceEdit batchEdit;
        batchEdit.Add(reparentEdit);
        if (_layer->CanApply(batchEdit)) {
            _layer->Apply(batchEdit);
            return true;
        }
        return false;
    }

    SdfLayerHandle _layer;
    SdfPath _source;
    SdfPath _destination;
};

/// TODO: how to avoid having to write the argument list ? it's the same as the constructor arguments
template void ExecuteAfterDraw<PrimNew>(SdfLayerRefPtr layer, std::string newName);
template void ExecuteAfterDraw<PrimNew>(SdfPrimSpecHandle primSpec, std::string newName);
template void ExecuteAfterDraw<PrimRemove>(SdfPrimSpecHandle primSpec);
template void ExecuteAfterDraw<PrimCreateCompositionArc>(SdfPrimSpecHandle primSpec, CompositionOperation operation,
                                                         CompositionList composition, std::string reference,
                                                         SdfPath targetPrimPath);
template void ExecuteAfterDraw<PrimReparent>(SdfLayerHandle layer, SdfPath source, SdfPath destination);

template void ExecuteAfterDraw<PrimCreateReference>(SdfPrimSpecHandle primSpec, int operation, SdfReference reference);
template void ExecuteAfterDraw<PrimCreatePayload>(SdfPrimSpecHandle primSpec, int operation, SdfPayload payload);
template void ExecuteAfterDraw<PrimCreateInherit>(SdfPrimSpecHandle primSpec, int operation, SdfPath inherit);
template void ExecuteAfterDraw<PrimCreateSpecialize>(SdfPrimSpecHandle primSpec, int operation, SdfPath specialize);