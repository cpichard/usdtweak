

#include <algorithm>
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
    PrimNew(SdfLayerRefPtr layer, std::string primName) : _layer(layer), _primName(std::move(primName)) {}

    // Create a child prim
    PrimNew(SdfLayerHandle layer, SdfPath parentPath, std::string primName)
        : _parentLayer(std::move(layer)), _parentPath(std::move(parentPath)), _primName(std::move(primName)) {}

    ~PrimNew() override {}

    bool DoIt() override {
        if (_layer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _layer);
            _newPrimSpec = SdfPrimSpec::New(_layer, _primName, SdfSpecifier::SdfSpecifierDef);
            _layer->InsertRootPrim(_newPrimSpec);
            return true;
        } else if (_parentLayer) {
            auto parentSpec = _parentLayer->GetPrimAtPath(_parentPath);
            if (!parentSpec) return false;
            SdfCommandGroupRecorder recorder(_undoCommands, _parentLayer);
            _newPrimSpec = SdfPrimSpec::New(parentSpec, _primName, SdfSpecifier::SdfSpecifierDef);
            return true;
        }
        return false;
    }

    SdfPrimSpecHandle _newPrimSpec;
    SdfLayerRefPtr _layer;       // set for root prim creation
    SdfLayerHandle _parentLayer; // set for child prim creation
    SdfPath _parentPath;
    std::string _primName;
};

struct PrimRemove : public SdfLayerCommand {

    PrimRemove(SdfLayerHandle layer, SdfPath path) : _layer(std::move(layer)), _paths({std::move(path)}) {}
    PrimRemove(SdfLayerHandle layer, std::vector<SdfPath> paths) : _layer(std::move(layer)), _paths(std::move(paths)) {}

    ~PrimRemove() override {}

    bool DoIt() override {
        if (!_layer) return false;

        // Sort shallowest paths first: removing a parent removes its children,
        // so child paths will resolve to null and are skipped automatically.
        std::sort(_paths.begin(), _paths.end(), [](const SdfPath &a, const SdfPath &b) {
            return a.GetPathElementCount() < b.GetPathElementCount();
        });

        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        bool result = false;
        for (const auto &path : _paths) {
            auto primSpec = _layer->GetPrimAtPath(path);
            if (!primSpec) continue;
            if (primSpec->GetNameParent()) {
                // Case where the prim is a variant
                if (path.IsPrimVariantSelectionPath()) {
                    auto variantSelection = path.GetVariantSelection();
                    TF_FOR_ALL(variantSet, primSpec->GetNameParent()->GetVariantSets()) {
                        if (variantSet->first == variantSelection.first) {
                            SdfVariantSetSpecHandle variantSetSpec = variantSet->second;
                            SdfVariantSpecHandle variantSpec = variantSetSpec->GetVariants().get(variantSelection.second);
                            if (variantSpec) {
                                variantSetSpec->RemoveVariant(variantSpec);
                                result = true;
                            }
                        }
                    }
                } else {
                    result |= primSpec->GetNameParent()->RemoveNameChild(primSpec);
                }
            } else {
                _layer->RemoveRootPrim(primSpec);
                result = true;
            }
        }
        return result;
    }

    SdfLayerHandle _layer;
    std::vector<SdfPath> _paths;
};

template <typename ItemType> struct PrimCreateListEditorOperation : SdfLayerCommand {
    PrimCreateListEditorOperation(SdfLayerHandle layer, SdfPath primPath, SdfListOpType operation, typename ItemType::value_type item)
        : _layer(std::move(layer)), _primPath(std::move(primPath)), _operation(operation), _item(std::move(item)) {}
    ~PrimCreateListEditorOperation() override {}

    bool DoIt() override {
        auto primSpec = _layer ? _layer->GetPrimAtPath(_primPath) : SdfPrimSpecHandle();
        if (primSpec) {
            SdfCommandGroupRecorder recorder(_undoCommands, primSpec->GetLayer());
            CreateListEditorOperation(GetListEditor(primSpec), _operation, _item);
            return true;
        }
        return false;
    }

    // Forced to inherit as the Specialize and Inherit arcs have the same type
    virtual SdfListEditorProxy<ItemType> GetListEditor(const SdfPrimSpecHandle &primSpec) = 0;

    SdfLayerHandle _layer;
    SdfPath _primPath;
    SdfListOpType _operation;
    typename ItemType::value_type _item;
};

struct PrimCreateReference : public PrimCreateListEditorOperation<SdfReferenceTypePolicy> {
    using PrimCreateListEditorOperation<SdfReferenceTypePolicy>::PrimCreateListEditorOperation;
    SdfReferencesProxy GetListEditor(const SdfPrimSpecHandle &primSpec) override { return primSpec->GetReferenceList(); }
};

struct PrimCreatePayload : public PrimCreateListEditorOperation<SdfPayloadTypePolicy> {
    using PrimCreateListEditorOperation<SdfPayloadTypePolicy>::PrimCreateListEditorOperation;
    SdfPayloadsProxy GetListEditor(const SdfPrimSpecHandle &primSpec) override { return primSpec->GetPayloadList(); }
};

struct PrimCreateInherit : public PrimCreateListEditorOperation<SdfPathKeyPolicy> {
    using PrimCreateListEditorOperation<SdfPathKeyPolicy>::PrimCreateListEditorOperation;
    SdfInheritsProxy GetListEditor(const SdfPrimSpecHandle &primSpec) override { return primSpec->GetInheritPathList(); }
};

struct PrimCreateSpecialize : public PrimCreateListEditorOperation<SdfPathKeyPolicy> {
    using PrimCreateListEditorOperation<SdfPathKeyPolicy>::PrimCreateListEditorOperation;
    SdfSpecializesProxy GetListEditor(const SdfPrimSpecHandle &primSpec) override { return primSpec->GetSpecializesList(); }
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

    PrimCreateAttribute(SdfLayerHandle layer, SdfPath primPath, std::string name, SdfValueTypeName typeName,
                        SdfVariability variability = SdfVariabilityVarying, bool custom = false, bool createDefault = false)
        : _layer(std::move(layer)), _primPath(std::move(primPath)), _name(std::move(name)), _typeName(std::move(typeName)),
          _variability(variability), _custom(custom), _createDefault(createDefault) {}

    ~PrimCreateAttribute() override {}

    bool DoIt() override {
        auto owner = _layer ? _layer->GetPrimAtPath(_primPath) : SdfPrimSpecHandle();
        if (!owner) return false;
        SdfCommandGroupRecorder recorder(_undoCommands, owner->GetLayer());
        if (SdfAttributeSpecHandle attribute = SdfAttributeSpec::New(owner, _name, _typeName, _variability, _custom)) {
            // Default value for now
            if (_createDefault) {
                auto defaultValue = _typeName.GetDefaultValue();
                attribute->SetDefaultValue(defaultValue);
            }
            return true;
        }
        return false;
    }

    SdfLayerHandle _layer;
    SdfPath _primPath;
    std::string _name;
    SdfValueTypeName _typeName = SdfValueTypeNames->Float;
    SdfVariability _variability = SdfVariabilityVarying;
    bool _custom = false;
    bool _createDefault = false;
};

struct PrimCreateRelationship : public SdfLayerCommand {

    PrimCreateRelationship(SdfLayerHandle layer, SdfPath primPath, std::string name, SdfVariability variability, bool custom,
                           SdfListOpType operation, std::string targetPath)
        : _layer(std::move(layer)), _primPath(std::move(primPath)), _name(std::move(name)), _variability(variability),
          _custom(custom), _operation(operation), _targetPath(targetPath) {}

    ~PrimCreateRelationship() override {}

    bool DoIt() override {
        auto owner = _layer ? _layer->GetPrimAtPath(_primPath) : SdfPrimSpecHandle();
        if (!owner) return false;
        SdfCommandGroupRecorder recorder(_undoCommands, owner->GetLayer());
        // TODO we could pass a list of space separated target and link them all
        if (SdfRelationshipSpecHandle relationship = SdfRelationshipSpec::New(owner, _name, _custom, _variability)) {
            CreateListEditorOperation(relationship->GetTargetPathList(), _operation, SdfPath(_targetPath));
            return true;
        }
        return false;
    }

    SdfLayerHandle _layer;
    SdfPath _primPath;
    std::string _name;
    SdfVariability _variability = SdfVariabilityVarying;
    bool _custom = false;
    SdfListOpType _operation = SdfListOpTypeExplicit;
    std::string _targetPath;
};

struct PrimReorder : public SdfLayerCommand {
    PrimReorder(SdfLayerHandle layer, SdfPath primPath, bool up) : _layer(std::move(layer)), _primPath(std::move(primPath)), _up(up) {}
    ~PrimReorder() override {}
    bool DoIt() override {
        if (!_layer) return false;
        auto prim = _layer->GetPrimAtPath(_primPath);
        if (!prim) return false;

        TfToken name = prim->GetNameToken();
        // Look for parent .. layer or prim
        // and find the position of the prim in the parent
        int position = -1;
        auto parent = prim->GetNameParent();
        const auto &nameChildren = parent ? parent->GetNameChildren() : _layer->GetRootPrims();
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

        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        SdfNamespaceEdit reorderEdit = SdfNamespaceEdit::Reorder(_primPath, position);
        SdfBatchNamespaceEdit batchEdit;
        batchEdit.Add(reorderEdit);
        if (_layer->CanApply(batchEdit)) {
            _layer->Apply(batchEdit);
            return true;
        }
        return false;
    }

    SdfLayerHandle _layer;
    SdfPath _primPath;
    bool _up = true;
};

struct PrimDuplicate : public SdfLayerCommand {
    PrimDuplicate(SdfLayerHandle layer, SdfPath primPath, std::string &oldName)
        : _layer(std::move(layer)), _primPath(std::move(primPath)), _newName(FindNextAvailableTokenString(oldName)){};
    ~PrimDuplicate() override {}
    bool DoIt() override {
        if (!_layer) return false;
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        return SdfCopySpec(_layer, _primPath, _layer, _primPath.ReplaceName(TfToken(_newName)));
    }

    SdfLayerHandle _layer;
    SdfPath _primPath;
    std::string _newName;
};

struct PrimAddBlueprint : public SdfLayerCommand {
    PrimAddBlueprint(SdfLayerHandle layer, SdfPath primPath, std::string primName, std::string blueprintPath)
        : _layer(std::move(layer)), _primPath(std::move(primPath)), _primName(std::move(primName)),
          _blueprintPath(std::move(blueprintPath)){};
    ~PrimAddBlueprint() override {}
    bool DoIt() override {
        if (!_layer) return false;
        auto prim = _layer->GetPrimAtPath(_primPath);
        if (!prim) return false;
        // Open a layer and copy the content of it onto this prim
        auto layerSource = SdfLayer::FindOrOpen(_blueprintPath);
        if (!layerSource)
            return false; // warning ??
        auto primSourcePath = SdfPath::AbsoluteRootPath().AppendChild(layerSource->GetDefaultPrim());
        // TODO check primSourcePath
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        // TODO check the GetDefaultPrim is available or create a new name
        auto primDest = SdfPrimSpec::New(prim, FindNextAvailableTokenString(layerSource->GetDefaultPrim()),
                                         SdfSpecifier::SdfSpecifierDef);
        return SdfCopySpec(layerSource, primSourcePath, _layer, primDest->GetPath());
        // Close the layer
    }

    std::string _primName;
    std::string _blueprintPath;
    SdfLayerHandle _layer;
    SdfPath _primPath;
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
    PrimCopy(SdfLayerHandle layer, SdfPath primPath) : _layer(std::move(layer)), _primPath(std::move(primPath)){};
    ~PrimCopy() override {}
    bool DoIt() override {
        if (_layer && _copyPasteLayer) {
            auto prim = _layer->GetPrimAtPath(_primPath);
            if (!prim) return false;
            SdfCommandGroupRecorder recorder(_undoCommands, _copyPasteLayer);
            // Ditch root prim
            const SdfPath CopiedPrimRoot = SdfPath::AbsoluteRootPath().AppendChild(GetCopyRoot());
            auto defaultPrim = _copyPasteLayer->GetPrimAtPath(CopiedPrimRoot);
            if (defaultPrim) {
                _copyPasteLayer->RemoveRootPrim(defaultPrim);
            }
            _copyPasteLayer->InsertRootPrim(SdfPrimSpec::New(_copyPasteLayer, GetCopyRoot().GetString(), SdfSpecifierDef));

            // Copy
            const bool copyOk = SdfCopySpec(_layer, _primPath, _copyPasteLayer,
                                            CopiedPrimRoot.AppendChild(_primPath.GetNameToken()));

            return copyOk;
        }
        return false;
    }
    SdfLayerHandle _layer;
    SdfPath _primPath;
};

struct PrimPaste : public CopyPasteCommand {
    PrimPaste(SdfLayerHandle layer, SdfPath primPath) : _layer(std::move(layer)), _primPath(std::move(primPath)){};
    ~PrimPaste() override {}
    bool DoIt() override {
        if (_layer && _copyPasteLayer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _layer);
            const SdfPath CopiedPrimRoot = SdfPath::AbsoluteRootPath().AppendChild(GetCopyRoot());
            auto defaultPrim = _copyPasteLayer->GetPrimAtPath(CopiedPrimRoot);
            if (defaultPrim) {
                for (const auto &child : defaultPrim->GetNameChildren()) {
                    // TODO: it might be better to do it in batch
                    if (!SdfCopySpec(_copyPasteLayer, child->GetPath(), _layer,
                                     _primPath.AppendChild(child->GetNameToken()))) {
                        return false;
                    }
                }
            }
            return true;
        }
        return false;
    }
    SdfLayerHandle _layer;
    SdfPath _primPath;
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
    PropertyPaste(SdfLayerHandle layer, SdfPath primPath) : _layer(std::move(layer)), _primPath(std::move(primPath)){};
    ~PropertyPaste() override {}
    bool DoIt() override {
        if (_layer && _copyPasteLayer) {
            SdfCommandGroupRecorder recorder(_undoCommands, _layer);
            const SdfPath CopiedPropertiesRoot = SdfPath::AbsoluteRootPath().AppendChild(GetCopyRoot());
            auto defaultPrim = _copyPasteLayer->GetPrimAtPath(CopiedPropertiesRoot);
            if (defaultPrim) {
                for (const auto &prop : defaultPrim->GetProperties()) {
                    // TODO: it might be better to do it in batch
                    if (!SdfCopySpec(_copyPasteLayer, prop->GetPath(), _layer,
                                     _primPath.AppendProperty(prop->GetNameToken()))) {
                        return false;
                    }
                }
            }
            return true;
        }
        return false;
    }
    SdfLayerHandle _layer;
    SdfPath _primPath;
};
template void ExecuteAfterDraw<PropertyPaste>(SdfLayerHandle layer, SdfPath primPath);

/// TODO: how to avoid having to write the argument list ? it's the same as the constructor arguments
template void ExecuteAfterDraw<PrimNew>(SdfLayerRefPtr layer, std::string newName);
template void ExecuteAfterDraw<PrimNew>(SdfLayerHandle layer, SdfPath parentPath, std::string newName);
template void ExecuteAfterDraw<PrimRemove>(SdfLayerHandle layer, SdfPath path);
template void ExecuteAfterDraw<PrimRemove>(SdfLayerHandle layer, std::vector<SdfPath> paths);
template void ExecuteAfterDraw<PrimReparent>(SdfLayerHandle layer, SdfPath source, SdfPath destination);
template void ExecuteAfterDraw<PrimReparent>(SdfLayerHandle layer, std::vector<SdfPath> source, SdfPath destination);
template void ExecuteAfterDraw<PrimCreateReference>(SdfLayerHandle layer, SdfPath primPath, SdfListOpType operation, SdfReference reference);
template void ExecuteAfterDraw<PrimCreatePayload>(SdfLayerHandle layer, SdfPath primPath, SdfListOpType operation, SdfPayload payload);
template void ExecuteAfterDraw<PrimCreateInherit>(SdfLayerHandle layer, SdfPath primPath, SdfListOpType operation, SdfPath inherit);
template void ExecuteAfterDraw<PrimCreateSpecialize>(SdfLayerHandle layer, SdfPath primPath, SdfListOpType operation, SdfPath specialize);
template void ExecuteAfterDraw<PrimCreateAttribute>(SdfLayerHandle layer, SdfPath primPath, std::string name, SdfValueTypeName typeName,
                                                    SdfVariability variability, bool custom, bool createDefault);
template void ExecuteAfterDraw<PrimCreateRelationship>(SdfLayerHandle layer, SdfPath primPath, std::string name, SdfVariability variability,
                                                       bool custom, SdfListOpType operation, std::string targetPath);
template void ExecuteAfterDraw<PrimReorder>(SdfLayerHandle layer, SdfPath primPath, bool up);
template void ExecuteAfterDraw<PrimDuplicate>(SdfLayerHandle layer, SdfPath primPath, std::string newName);
template void ExecuteAfterDraw<PrimAddBlueprint>(SdfLayerHandle layer, SdfPath primPath, std::string primName, std::string bluePrintPath);
template void ExecuteAfterDraw<PrimCopy>(SdfLayerHandle layer, SdfPath primPath);
template void ExecuteAfterDraw<PrimPaste>(SdfLayerHandle layer, SdfPath primPath);
template void ExecuteAfterDraw<PrimCreateAttributeConnection>(SdfAttributeSpecHandle attr, SdfListOpType operation,
                                                              std::string connectionEndPoint);
