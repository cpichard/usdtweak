

#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/namespaceEdit.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/valueTypeName.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include "CommandsImpl.h"
#include "SdfUndoRedoRecorder.h"
#include "UsdHelpers.h"

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
            SdfCommandGroupRecorder recorder(_undoCommands, _layer);
            _newPrimSpec = SdfPrimSpec::New(_layer, _primName, SdfSpecifier::SdfSpecifierDef);
            _layer->InsertRootPrim(_newPrimSpec);
            return true;
        } else {
            SdfCommandGroupRecorder recorder(_undoCommands, _primSpec->GetLayer());
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
        SdfCommandGroupRecorder recorder(_undoCommands, layer);
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

template <typename ItemType> struct PrimCreateListEditorOperation : SdfLayerCommand {
    PrimCreateListEditorOperation(SdfPrimSpecHandle primSpec, SdfListOpType operation, typename ItemType::value_type item)
        : _primSpec(primSpec), _operation(operation), _item(std::move(item)) {}
    ~PrimCreateListEditorOperation() override {}

    bool DoIt() override {
        if (_primSpec) {
            SdfCommandGroupRecorder recorder(_undoCommands, _primSpec->GetLayer());
            CreateListEditorOperation(GetListEditor(), _operation, _item);
            return true;
        }
        return false;
    }

    // Forced to inherit as the Specialize and Inherit arcs have the same type
    virtual SdfListEditorProxy<ItemType> GetListEditor() = 0;

    SdfPrimSpecHandle _primSpec;
    SdfListOpType _operation;
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

struct PrimReparent : public SdfLayerCommand {
    PrimReparent(SdfLayerHandle layer, SdfPath source, SdfPath destination)
        : _layer(std::move(layer)), _source{source}, _destination(destination) {}

    PrimReparent(SdfLayerHandle layer, std::vector<SdfPath> source, SdfPath destination)
        : _layer(std::move(layer)), _source(std::move(source)), _destination(destination) {
        // Move the farthest paths first
        std::sort(_source.begin(), _source.end(),
                  [](const SdfPath &a, const SdfPath &b) { return a.GetString().size() > b.GetString().size(); });
    }

    ~PrimReparent() override {}

    bool DoIt() override {
        if (!_layer)
            return false;
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        SdfBatchNamespaceEdit batchEdit;
        for (const auto &source : _source) {
            SdfNamespaceEdit reparentEdit = SdfNamespaceEdit::Reparent(source, _destination, 0);
            batchEdit.Add(reparentEdit);
        }
        SdfNamespaceEditDetailVector details;
        if (_layer->CanApply(batchEdit, &details)) {
            _layer->Apply(batchEdit);
            return true;
        } else { // TODO in a USD log
            std::cout << "Unable to reparent, reasons are:" << std::endl;
            for (const auto &detail : details) {
                std::cout << detail.edit.currentPath.GetString() << " " << detail.reason << std::endl;
            }
        }
        return false;
    }

    SdfLayerHandle _layer;
    std::vector<SdfPath> _source;
    SdfPath _destination;
};

struct PrimCreateAttribute : public SdfLayerCommand {

    PrimCreateAttribute(SdfPrimSpecHandle owner, std::string name, SdfValueTypeName typeName,
                        SdfVariability variability = SdfVariabilityVarying, bool custom = false, bool createDefault = false)
        : _owner(std::move(owner)), _name(std::move(name)), _typeName(std::move(typeName)), _variability(variability),
          _custom(custom), _createDefault(createDefault) {}

    ~PrimCreateAttribute() override {}

    bool DoIt() override {
        if (!_owner)
            return false;
        auto layer = _owner->GetLayer();
        SdfCommandGroupRecorder recorder(_undoCommands, layer);
        if (SdfAttributeSpecHandle attribute = SdfAttributeSpec::New(_owner, _name, _typeName, _variability, _custom)) {
            // Default value for now
            if (_createDefault) {
                auto defaultValue = _typeName.GetDefaultValue();
                attribute->SetDefaultValue(defaultValue);
            }
            return true;
        }
        return false;
    }
    //
    SdfPrimSpecHandle _owner;
    std::string _name;
    SdfValueTypeName _typeName = SdfValueTypeNames->Float;
    SdfVariability _variability = SdfVariabilityVarying;
    bool _custom = false;
    bool _createDefault = false;
};

struct PrimCreateRelationship : public SdfLayerCommand {

    PrimCreateRelationship(SdfPrimSpecHandle owner, std::string name, SdfVariability variability, bool custom,
                           SdfListOpType operation, std::string targetPath)
        : _owner(std::move(owner)), _name(std::move(name)), _variability(variability), _custom(custom), _operation(operation),
          _targetPath(targetPath) {}

    ~PrimCreateRelationship() override {}

    bool DoIt() override {
        if (!_owner)
            return false;
        auto layer = _owner->GetLayer();
        SdfCommandGroupRecorder recorder(_undoCommands, layer);
        if (SdfRelationshipSpecHandle relationship = SdfRelationshipSpec::New(_owner, _name, _custom, _variability)) {
            CreateListEditorOperation(relationship->GetTargetPathList(), _operation, SdfPath(_targetPath));
            return true;
        }
        return false;
    }
    //
    SdfPrimSpecHandle _owner;
    std::string _name;
    SdfVariability _variability = SdfVariabilityVarying;
    bool _custom = false;
    SdfListOpType _operation = SdfListOpTypeExplicit;
    std::string _targetPath;
};

struct PrimReorder : public SdfLayerCommand {
    PrimReorder(SdfPrimSpecHandle prim, bool up) : _prim(std::move(prim)), _up(up) {}
    ~PrimReorder() override {}
    bool DoIt() override {
        if (!_prim)
            return false;
        auto layer = _prim->GetLayer();

        TfToken name = _prim->GetNameToken();
        // Look for parent .. layer or prim
        // and find the position of the prim in the parent
        int position = -1;
        auto parent = _prim->GetNameParent();
        const auto &nameChildren = parent ? parent->GetNameChildren() : _prim->GetLayer()->GetRootPrims();
        for (int i = 0; i < nameChildren.size(); ++i) {
            if (nameChildren[i]->GetNameToken() == name) {
                position = i;
                break;
            }
        }

        if (position == -1)
            return false;
        position = _up ? position - 1 : position + 2;
        if (position < 0 || position > nameChildren.size())
            return false;

        SdfCommandGroupRecorder recorder(_undoCommands, layer);
        SdfNamespaceEdit reorderEdit = SdfNamespaceEdit::Reorder(_prim->GetPath(), position);
        SdfBatchNamespaceEdit batchEdit;
        batchEdit.Add(reorderEdit);
        if (layer->CanApply(batchEdit)) {
            layer->Apply(batchEdit);
            return true;
        }
        return false;
    }

    bool _up = true;
    SdfPrimSpecHandle _prim;
};

struct PrimDuplicate : public SdfLayerCommand {
    PrimDuplicate(SdfPrimSpecHandle prim, std::string &newName) : _prim(std::move(prim)), _newName(newName){};
    ~PrimDuplicate() override {}
    bool DoIt() override {
        if (_prim) {
            SdfCommandGroupRecorder recorder(_undoCommands, _prim->GetLayer());
            return (SdfCopySpec(_prim->GetLayer(), _prim->GetPath(), _prim->GetLayer(),
                                _prim->GetPath().ReplaceName(TfToken(_newName))));
        }
        return false;
    }

    std::string _newName;
    SdfPrimSpecHandle _prim;
};

// A base class for copy/paste commands, it keeps the copy/paste layer and
// used paths
struct CopyPasteCommand : public SdfLayerCommand {
    ~CopyPasteCommand() override {}
    TfToken GetCopyRoot() const { return TfToken("Copy"); }
    static SdfLayerRefPtr _copyPasteLayer;
};
SdfLayerRefPtr CopyPasteCommand::_copyPasteLayer(SdfLayer::CreateAnonymous("CopyPasteBuffer"));

struct PrimCopy : public CopyPasteCommand {
    PrimCopy(SdfPrimSpecHandle prim) : _prim(prim){};
    ~PrimCopy() override {}
    bool DoIt() override {
        if (_prim && _copyPasteLayer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _copyPasteLayer);
            // Ditch root prim
            const SdfPath CopiedPrimRoot = SdfPath::AbsoluteRootPath().AppendChild(GetCopyRoot());
            auto defaultPrim = _copyPasteLayer->GetPrimAtPath(CopiedPrimRoot);
            if (defaultPrim) {
                _copyPasteLayer->RemoveRootPrim(defaultPrim);
            }
            _copyPasteLayer->InsertRootPrim(SdfPrimSpec::New(_copyPasteLayer, GetCopyRoot().GetString(), SdfSpecifierDef));

            // Copy
            const bool copyOk = SdfCopySpec(_prim->GetLayer(), _prim->GetPath(), _copyPasteLayer,
                                            CopiedPrimRoot.AppendChild(_prim->GetNameToken()));

            return copyOk;
        }
        return false;
    }
    SdfPrimSpecHandle _prim;
};

struct PrimPaste : public CopyPasteCommand {
    PrimPaste(SdfPrimSpecHandle prim) : _prim(prim){};
    ~PrimPaste() override {}
    bool DoIt() override {
        if (_prim && _copyPasteLayer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _prim->GetLayer());
            const SdfPath CopiedPrimRoot = SdfPath::AbsoluteRootPath().AppendChild(GetCopyRoot());
            auto defaultPrim = _copyPasteLayer->GetPrimAtPath(CopiedPrimRoot);
            if (defaultPrim) {
                for (const auto &child : defaultPrim->GetNameChildren()) {
                    // TODO: it might be better to do it in batch
                    if (!SdfCopySpec(_copyPasteLayer, child->GetPath(), _prim->GetLayer(),
                                     _prim->GetPath().AppendChild(child->GetNameToken()))) {
                        return false;
                    }
                }
            }
            return true;
        }
        return false;
    }
    SdfPrimSpecHandle _prim;
};

struct PrimCreateAttributeConnection : public SdfLayerCommand {
    PrimCreateAttributeConnection(SdfAttributeSpecHandle attr, SdfListOpType operation, std::string connectionEndPoint)
        : _attr(attr), _operation(operation), _connectionEndPoint(connectionEndPoint) {}
    ~PrimCreateAttributeConnection() override {}
    bool DoIt() override {
        if (_attr) {
            SdfCommandGroupRecorder recorder(_undoCommands, _attr->GetLayer());
            CreateListEditorOperation(_attr->GetConnectionPathList(), _operation, _connectionEndPoint);
            return true;
        }

        return true;
    }

    SdfAttributeSpecHandle _attr;
    SdfListOpType _operation = SdfListOpTypeExplicit;
    SdfPath _connectionEndPoint;
};

struct PropertyCopy : public CopyPasteCommand {
    PropertyCopy(SdfPropertySpecHandle prop) : _prop(prop){};
    ~PropertyCopy() override {}
    bool DoIt() override {
        if (_prop && _copyPasteLayer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _copyPasteLayer);
            const SdfPath copiedPropertiesRoot = SdfPath::AbsoluteRootPath().AppendChild(GetCopyRoot());
            auto copiedPropertiesPrim = _copyPasteLayer->GetPrimAtPath(copiedPropertiesRoot);
            if (copiedPropertiesPrim) {
                _copyPasteLayer->RemoveRootPrim(copiedPropertiesPrim);
            }
            _copyPasteLayer->InsertRootPrim(SdfPrimSpec::New(_copyPasteLayer, GetCopyRoot().GetString(), SdfSpecifierDef));

            // Copy
            const bool copyOk = SdfCopySpec(_prop->GetLayer(), _prop->GetPath(), _copyPasteLayer,
                                            copiedPropertiesRoot.AppendProperty(_prop->GetNameToken()));
            return copyOk;
        }
        return false;
    }
    SdfPropertySpecHandle _prop;
};
template void ExecuteAfterDraw<PropertyCopy>(SdfPropertySpecHandle prop);

struct PropertyPaste : public CopyPasteCommand {
    PropertyPaste(SdfPrimSpecHandle prim) : _prim(prim){};
    ~PropertyPaste() override {}
    bool DoIt() override {
        if (_prim && _copyPasteLayer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _prim->GetLayer());
            const SdfPath CopiedPropertiesRoot = SdfPath::AbsoluteRootPath().AppendChild(GetCopyRoot());
            auto defaultPrim = _copyPasteLayer->GetPrimAtPath(CopiedPropertiesRoot);
            if (defaultPrim) {
                for (const auto &prop : defaultPrim->GetProperties()) {
                    // TODO: it might be better to do it in batch
                    if (!SdfCopySpec(_copyPasteLayer, prop->GetPath(), _prim->GetLayer(),
                                     _prim->GetPath().AppendProperty(prop->GetNameToken()))) {
                        return false;
                    }
                }
            }
            return true;
        }
        return false;
    }
    SdfPrimSpecHandle _prim;
};
template void ExecuteAfterDraw<PropertyPaste>(SdfPrimSpecHandle prim);

/// TODO: how to avoid having to write the argument list ? it's the same as the constructor arguments
template void ExecuteAfterDraw<PrimNew>(SdfLayerRefPtr layer, std::string newName);
template void ExecuteAfterDraw<PrimNew>(SdfPrimSpecHandle primSpec, std::string newName);
template void ExecuteAfterDraw<PrimRemove>(SdfPrimSpecHandle primSpec);
template void ExecuteAfterDraw<PrimReparent>(SdfLayerHandle layer, SdfPath source, SdfPath destination);
template void ExecuteAfterDraw<PrimReparent>(SdfLayerHandle layer, std::vector<SdfPath> source, SdfPath destination);
template void ExecuteAfterDraw<PrimCreateReference>(SdfPrimSpecHandle primSpec, SdfListOpType operation, SdfReference reference);
template void ExecuteAfterDraw<PrimCreatePayload>(SdfPrimSpecHandle primSpec, SdfListOpType operation, SdfPayload payload);
template void ExecuteAfterDraw<PrimCreateInherit>(SdfPrimSpecHandle primSpec, SdfListOpType operation, SdfPath inherit);
template void ExecuteAfterDraw<PrimCreateSpecialize>(SdfPrimSpecHandle primSpec, SdfListOpType operation, SdfPath specialize);
template void ExecuteAfterDraw<PrimCreateAttribute>(SdfPrimSpecHandle owner, std::string name, SdfValueTypeName typeName,
                                                    SdfVariability variability, bool custom, bool createDefault);
template void ExecuteAfterDraw<PrimCreateRelationship>(SdfPrimSpecHandle owner, std::string name, SdfVariability variability,
                                                       bool custom, SdfListOpType operation, std::string targetPath);
template void ExecuteAfterDraw<PrimReorder>(SdfPrimSpecHandle owner, bool up);
template void ExecuteAfterDraw<PrimDuplicate>(SdfPrimSpecHandle prim, std::string newName);
template void ExecuteAfterDraw<PrimCopy>(SdfPrimSpecHandle prim);
template void ExecuteAfterDraw<PrimPaste>(SdfPrimSpecHandle prim);
template void ExecuteAfterDraw<PrimCreateAttributeConnection>(SdfAttributeSpecHandle attr, SdfListOpType operation,
                                                              std::string connectionEndPoint);
