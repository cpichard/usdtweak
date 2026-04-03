

#include <algorithm>
#include <unordered_map>
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
    PrimReorder(SdfLayerHandle layer, std::vector<SdfPath> primPaths, bool up)
        : _layer(std::move(layer)), _primPaths(std::move(primPaths)), _up(up) {}
    ~PrimReorder() override {}
    bool DoIt() override {
        if (!_layer || _primPaths.empty()) return false;

        // Handle variant reordering: SdfNamespaceEdit doesn't work for variant paths
        if (_primPaths[0].IsPrimVariantSelectionPath()) {
            std::unordered_map<SdfPath, std::vector<TfToken>, SdfPath::Hash> variantGroups;
            for (const auto &path : _primPaths) {
                if (!path.IsPrimVariantSelectionPath()) continue;
                auto sel = path.GetVariantSelection();
                SdfPath variantSetPath = path.GetParentPath().AppendVariantSelection(TfToken(sel.first), TfToken(""));
                variantGroups[variantSetPath].emplace_back(sel.second);
            }
            SdfCommandGroupRecorder recorder(_undoCommands, _layer);
            bool anyEdit = false;
            for (auto &[variantSetPath, selectedVariants] : variantGroups) {
                auto children = _layer->GetFieldAs<std::vector<TfToken>>(variantSetPath, SdfChildrenKeys->VariantChildren);
                const int n = (int)children.size();
                std::vector<std::pair<int, TfToken>> indexed;
                for (const auto &v : selectedVariants) {
                    for (int i = 0; i < n; ++i) {
                        if (children[i] == v) { indexed.emplace_back(i, v); break; }
                    }
                }
                if (indexed.empty()) continue;
                std::sort(indexed.begin(), indexed.end());
                if (_up && indexed.front().first == 0) continue;
                if (!_up && indexed.back().first == n - 1) continue;
                if (_up) {
                    for (auto &[idx, name] : indexed) { std::swap(children[idx], children[idx - 1]); }
                } else {
                    for (int i = (int)indexed.size() - 1; i >= 0; --i) {
                        std::swap(children[indexed[i].first], children[indexed[i].first + 1]);
                    }
                }
                _layer->GetStateDelegate()->SetField(variantSetPath, SdfChildrenKeys->VariantChildren, VtValue(children));
                anyEdit = true;
            }
            return anyEdit;
        }

        // Group paths by parent path so each sibling group is handled independently
        std::unordered_map<SdfPath, std::vector<SdfPath>, SdfPath::Hash> groups;
        for (const auto &path : _primPaths) {
            groups[path.GetParentPath()].push_back(path);
        }

        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        SdfBatchNamespaceEdit batchEdit;
        bool anyEdit = false;

        for (auto &[parentPath, paths] : groups) {
            auto parent = parentPath.IsAbsoluteRootPath() ? SdfPrimSpecHandle()
                                                          : _layer->GetPrimAtPath(parentPath);
            const auto &siblings = parent ? parent->GetNameChildren() : _layer->GetRootPrims();
            const int siblingCount = static_cast<int>(siblings.size());

            // Map each selected path to its current index in the sibling list
            std::vector<std::pair<int, SdfPath>> indexed; // (index, path)
            for (const auto &path : paths) {
                for (int i = 0; i < siblingCount; ++i) {
                    if (siblings[i]->GetPath() == path) {
                        indexed.emplace_back(i, path);
                        break;
                    }
                }
            }
            if (indexed.empty()) continue;

            // Sort ascending by position
            std::sort(indexed.begin(), indexed.end());

            // Boundary check: skip this group if the block can't move
            if (_up  && indexed.front().first == 0) continue;
            if (!_up && indexed.back().first == siblingCount - 1) continue;

            if (_up) {
                // Process topmost first (ascending): each moves one slot up
                for (const auto &[idx, path] : indexed) {
                    batchEdit.Add(SdfNamespaceEdit::Reorder(path, idx - 1));
                }
            } else {
                // Process bottommost first (descending): each moves one slot down
                for (int i = static_cast<int>(indexed.size()) - 1; i >= 0; --i) {
                    const auto &[idx, path] = indexed[i];
                    batchEdit.Add(SdfNamespaceEdit::Reorder(path, idx + 2));
                }
            }
            anyEdit = true;
        }

        if (anyEdit && _layer->CanApply(batchEdit)) {
            _layer->Apply(batchEdit);
            return true;
        }
        return false;
    }

    SdfLayerHandle _layer;
    std::vector<SdfPath> _primPaths;
    bool _up = true;
};

struct PrimDuplicate : public SdfLayerCommand {
    PrimDuplicate(SdfLayerHandle layer, std::vector<SdfPath> primPaths)
        : _layer(std::move(layer)), _primPaths(std::move(primPaths)){};
    ~PrimDuplicate() override {}
    bool DoIt() override {
        if (!_layer || _primPaths.empty()) return false;
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        // Compute names inside DoIt so each SdfCopySpec interns the new token
        // before the next FindNextAvailableTokenString call, avoiding inter-prim collisions.
        bool allOk = true;
        for (const auto &primPath : _primPaths) {
            if (primPath.IsPrimVariantSelectionPath()) {
                auto variantSelection = primPath.GetVariantSelection();
                auto parentPrimSpec = _layer->GetPrimAtPath(primPath.GetParentPath());
                if (!parentPrimSpec) { allOk = false; continue; }
                SdfVariantSetSpecHandle variantSetSpec;
                TF_FOR_ALL(it, parentPrimSpec->GetVariantSets()) {
                    if (it->first == variantSelection.first) { variantSetSpec = it->second; break; }
                }
                if (!variantSetSpec) { allOk = false; continue; }
                std::string newName = FindNextAvailableTokenString(variantSelection.second);
                while (variantSetSpec->GetVariants().get(newName)) {
                    newName = FindNextAvailableTokenString(newName);
                }
                auto newVariantSpec = SdfVariantSpec::New(variantSetSpec, newName);
                if (!newVariantSpec) { allOk = false; continue; }
                SdfPath newVariantPath = primPath.GetParentPath().AppendVariantSelection(
                    TfToken(variantSelection.first), TfToken(newName));
                if (!SdfCopySpec(_layer, primPath, _layer, newVariantPath)) {
                    variantSetSpec->RemoveVariant(newVariantSpec);
                    allOk = false;
                }
            } else {
                auto prim = _layer->GetPrimAtPath(primPath);
                if (!prim) { allOk = false; continue; }
                const std::string newName = FindNextAvailableTokenString(prim->GetName());
                if (!SdfCopySpec(_layer, primPath, _layer, primPath.ReplaceName(TfToken(newName)))) {
                    allOk = false;
                }
            }
        }
        return allOk;
    }

    SdfLayerHandle _layer;
    std::vector<SdfPath> _primPaths;
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
    PrimCopy(SdfLayerHandle layer, std::vector<SdfPath> primPaths) : _layer(std::move(layer)), _primPaths(std::move(primPaths)){};
    ~PrimCopy() override {}
    bool DoIt() override {
        if (_layer && _copyPasteLayer && !_primPaths.empty()) {
            SdfCommandGroupRecorder recorder(_undoCommands, _copyPasteLayer);
            // Ditch root prim
            const SdfPath CopiedPrimRoot = SdfPath::AbsoluteRootPath().AppendChild(GetCopyRoot());
            auto defaultPrim = _copyPasteLayer->GetPrimAtPath(CopiedPrimRoot);
            if (defaultPrim) {
                _copyPasteLayer->RemoveRootPrim(defaultPrim);
            }
            _copyPasteLayer->InsertRootPrim(SdfPrimSpec::New(_copyPasteLayer, GetCopyRoot().GetString(), SdfSpecifierDef));

            // Copy all selected prims
            bool allOk = true;
            for (const auto &primPath : _primPaths) {
                if (!_layer->GetPrimAtPath(primPath)) { allOk = false; continue; }
                if (!SdfCopySpec(_layer, primPath, _copyPasteLayer,
                                 CopiedPrimRoot.AppendChild(primPath.GetNameToken()))) {
                    allOk = false;
                }
            }
            return allOk;
        }
        return false;
    }
    SdfLayerHandle _layer;
    std::vector<SdfPath> _primPaths;
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

struct VariantNew : public SdfLayerCommand {
    VariantNew(SdfLayerHandle layer, SdfPath variantPath, std::string newName)
        : _layer(std::move(layer)), _variantPath(std::move(variantPath)), _newName(std::move(newName)) {}
    ~VariantNew() override {}
    bool DoIt() override {
        if (!_layer || !_variantPath.IsPrimVariantSelectionPath()) return false;
        auto variantSelection = _variantPath.GetVariantSelection();
        auto primSpec = _layer->GetPrimAtPath(_variantPath.GetParentPath());
        if (!primSpec) return false;
        SdfVariantSetSpecHandle variantSetSpec;
        TF_FOR_ALL(it, primSpec->GetVariantSets()) {
            if (it->first == variantSelection.first) { variantSetSpec = it->second; break; }
        }
        if (!variantSetSpec) return false;
        if (variantSetSpec->GetVariants().get(_newName)) return false;
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        return SdfVariantSpec::New(variantSetSpec, _newName) != nullptr;
    }
    SdfLayerHandle _layer;
    SdfPath _variantPath;
    std::string _newName;
};
template void ExecuteAfterDraw<VariantNew>(SdfLayerHandle layer, SdfPath variantPath, std::string newName);

struct VariantRename : public SdfLayerCommand {
    VariantRename(SdfLayerHandle layer, SdfPath variantPath, std::string newName)
        : _layer(std::move(layer)), _variantPath(std::move(variantPath)), _newName(std::move(newName)) {}
    ~VariantRename() override {}

    bool DoIt() override {
        if (!_layer || !_variantPath.IsPrimVariantSelectionPath()) return false;
        auto variantSelection = _variantPath.GetVariantSelection();
        const std::string &variantSetName = variantSelection.first;
        const std::string &oldName = variantSelection.second;
        if (oldName == _newName || _newName.empty()) return false;

        auto parentPath = _variantPath.GetParentPath();
        auto primSpec = _layer->GetPrimAtPath(parentPath);
        if (!primSpec) return false;

        // Find the variant set spec
        SdfVariantSetSpecHandle variantSetSpec;
        TF_FOR_ALL(it, primSpec->GetVariantSets()) {
            if (it->first == variantSetName) {
                variantSetSpec = it->second;
                break;
            }
        }
        if (!variantSetSpec) return false;

        SdfVariantSpecHandle oldVariantSpec = variantSetSpec->GetVariants().get(oldName);
        if (!oldVariantSpec) return false;

        // Reject if the new name already exists
        if (variantSetSpec->GetVariants().get(_newName)) return false;

        // Record the old variant's position before any mutations
        SdfPath variantSetPath = parentPath.AppendVariantSelection(TfToken(variantSetName), TfToken(""));
        int oldIndex = -1;
        {
            auto variantChildren = _layer->GetFieldAs<std::vector<TfToken>>(variantSetPath, SdfChildrenKeys->VariantChildren);
            for (int i = 0; i < (int)variantChildren.size(); ++i) {
                if (variantChildren[i] == TfToken(oldName)) {
                    oldIndex = i;
                    break;
                }
            }
        }

        SdfPath newVariantPath = parentPath.AppendVariantSelection(TfToken(variantSetName), TfToken(_newName));

        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        // Pre-create the new SdfVariantSpec so SdfCopySpec has a valid destination
        auto newVariantSpec = SdfVariantSpec::New(variantSetSpec, _newName);
        if (!newVariantSpec) return false;

        if (!SdfCopySpec(_layer, _variantPath, _layer, newVariantPath)) {
            variantSetSpec->RemoveVariant(newVariantSpec);
            return false;
        }
        variantSetSpec->RemoveVariant(oldVariantSpec);

        // Restore original position in the variant list
        if (oldIndex >= 0) {
            auto newChildren = _layer->GetFieldAs<std::vector<TfToken>>(variantSetPath, SdfChildrenKeys->VariantChildren);
            auto it = std::find(newChildren.begin(), newChildren.end(), TfToken(_newName));
            if (it != newChildren.end() && (int)std::distance(newChildren.begin(), it) != oldIndex) {
                newChildren.erase(it);
                newChildren.insert(newChildren.begin() + std::min(oldIndex, (int)newChildren.size()), TfToken(_newName));
                _layer->GetStateDelegate()->SetField(variantSetPath, SdfChildrenKeys->VariantChildren, VtValue(newChildren));
            }
        }
        return true;
    }

    SdfLayerHandle _layer;
    SdfPath _variantPath;
    std::string _newName;
};
template void ExecuteAfterDraw<VariantRename>(SdfLayerHandle layer, SdfPath variantPath, std::string newName);

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
template void ExecuteAfterDraw<PrimReorder>(SdfLayerHandle layer, std::vector<SdfPath> primPaths, bool up);
template void ExecuteAfterDraw<PrimDuplicate>(SdfLayerHandle layer, std::vector<SdfPath> primPaths);
template void ExecuteAfterDraw<PrimAddBlueprint>(SdfLayerHandle layer, SdfPath primPath, std::string primName, std::string bluePrintPath);
template void ExecuteAfterDraw<PrimCopy>(SdfLayerHandle layer, std::vector<SdfPath> primPaths);
template void ExecuteAfterDraw<PrimPaste>(SdfLayerHandle layer, SdfPath primPath);
template void ExecuteAfterDraw<PrimCreateAttributeConnection>(SdfAttributeSpecHandle attr, SdfListOpType operation,
                                                              std::string connectionEndPoint);
