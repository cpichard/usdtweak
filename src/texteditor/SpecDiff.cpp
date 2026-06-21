#include "SpecDiff.h"

#include <algorithm>
#include <set>

#include <pxr/base/tf/stringUtils.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/schema.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>

#include "UsdaWriter.h"

namespace {

/// Mirror of the writer's metadata-section predicates (fileIO_Common.h):
/// which authored fields the text serialization places in the ( ) block.
bool IsBaseMetadataField(SdfSpecType specType, const TfToken &field) {
    const SdfSchema::SpecDefinition *definition = SdfSchema::GetInstance().GetSpecDefinition(specType);
    return !definition->IsValidField(field) || definition->IsMetadataField(field);
}

bool IsPrimMetadataField(const TfToken &field) {
    if (field == SdfFieldKeys->TypeName) {
        return false;
    }
    return IsBaseMetadataField(SdfSpecTypePrim, field) || field == SdfFieldKeys->Payload ||
           field == SdfFieldKeys->References || field == SdfFieldKeys->Relocates ||
           field == SdfFieldKeys->InheritPaths || field == SdfFieldKeys->Specializes ||
           field == SdfFieldKeys->VariantSetNames || field == SdfFieldKeys->VariantSelection;
}

bool IsAttributeMetadataField(const TfToken &field) {
    return IsBaseMetadataField(SdfSpecTypeAttribute, field) || field == SdfFieldKeys->DisplayUnit;
}

bool IsRelationshipMetadataField(const TfToken &field) {
    return IsBaseMetadataField(SdfSpecTypeRelationship, field);
}

bool IsLayerMetadataField(const TfToken &field) {
    // SubLayerOffsets are serialized inside the subLayers entry
    return IsBaseMetadataField(SdfSpecTypePseudoRoot, field) || field == SdfFieldKeys->SubLayers ||
           field == SdfFieldKeys->SubLayerOffsets;
}

/// Fields the writer skips even when authored. isLayer: the layer writer has
/// extra skips (empty doc, false hasOwnedSubLayers).
bool WriterSkipsField(const SdfSpec &spec, const TfToken &field, bool isLayer = false) {
    const VtValue value = spec.GetField(field);
    if (field == SdfFieldKeys->Comment) {
        return value.IsHolding<std::string>() && value.UncheckedGet<std::string>().empty();
    }
    if (field == SdfFieldKeys->VariantSelection) {
        return value.IsHolding<SdfVariantSelectionMap>() &&
               value.UncheckedGet<SdfVariantSelectionMap>().empty();
    }
    if (isLayer) {
        if (field == SdfFieldKeys->Documentation) {
            return value.IsHolding<std::string>() && value.UncheckedGet<std::string>().empty();
        }
        if (field == SdfFieldKeys->HasOwnedSubLayers) {
            return value.IsHolding<bool>() && !value.UncheckedGet<bool>();
        }
    }
    return false;
}

struct ApplyState {
    bool dryRun = false;
    bool ok = true;
    std::vector<std::string> *errors = nullptr;

    void Error(const SdfPath &path, const std::string &message) {
        ok = false;
        if (errors) {
            errors->push_back(path.GetString() + ": " + message);
        }
    }
};

void ApplyMetadataToSpec(const ParsedMetadata &parsed, SdfSpec spec, const SdfPath &path,
                         bool (*isMetadataField)(const TfToken &), ApplyState &state,
                         bool isLayer = false) {
    // Clear authored metadata absent from the text (unknown fields untouched)
    const TfTokenVector fields = spec.ListFields();
    for (const TfToken &field : fields) {
        if (!isMetadataField(field) || WriterSkipsField(spec, field, isLayer) ||
            parsed.unknownFields.count(field)) {
            continue;
        }
        if (!parsed.fields.count(field) && !state.dryRun) {
            spec.ClearField(field);
        }
    }
    // Set differing fields
    for (const auto &entry : parsed.fields) {
        VtValue value = entry.second;
        if (entry.first == SdfFieldKeys->Relocates && value.IsHolding<SdfRelocatesMap>()) {
            SdfRelocatesMap anchored;
            for (const auto &relocate : value.UncheckedGet<SdfRelocatesMap>()) {
                anchored[relocate.first.MakeAbsolutePath(path)] =
                    relocate.second.MakeAbsolutePath(path);
            }
            value = VtValue(anchored);
        }
        if (spec.GetField(entry.first) != value && !state.dryRun) {
            spec.SetField(entry.first, value);
        }
    }
}

void ValidateNewProperty(const ParsedProperty &parsed, const SdfPath &ownerPath, ApplyState &state) {
    if (parsed.unsupportedValueType) {
        state.Error(ownerPath, "cannot create '" + parsed.name + "' with unregistered type '" +
                                   parsed.typeName + "'");
    } else if (!parsed.isRelationship &&
               !SdfSchema::GetInstance().FindType(TfToken(parsed.typeName))) {
        state.Error(ownerPath, "unknown attribute type '" + parsed.typeName + "'");
    }
    if (parsed.foldedDefault || !parsed.foldedSampleTimes.empty()) {
        state.Error(ownerPath, "new property '" + parsed.name +
                                   "' cannot be created from a folded placeholder");
    }
    if (!SdfPath::IsValidNamespacedIdentifier(parsed.name)) {
        state.Error(ownerPath, "'" + parsed.name + "' is not a valid property name");
    }
}

void ApplyPropertyContent(const ParsedProperty &parsed, const SdfPropertySpecHandle &handle,
                          ApplyState &state);

void CreatePropertyFromParsed(const ParsedProperty &parsed, const SdfPrimSpecHandle &owner,
                              ApplyState &state) {
    ValidateNewProperty(parsed, owner->GetPath(), state);
    if (state.dryRun || !state.ok) {
        return;
    }
    SdfPropertySpecHandle handle;
    if (parsed.isRelationship) {
        handle = SdfRelationshipSpec::New(owner, parsed.name, parsed.custom, parsed.variability);
    } else {
        handle = SdfAttributeSpec::New(owner, parsed.name,
                                       SdfSchema::GetInstance().FindType(TfToken(parsed.typeName)),
                                       parsed.variability, parsed.custom);
    }
    if (!handle) {
        state.Error(owner->GetPath(), "could not create property '" + parsed.name + "'");
        return;
    }
    ApplyPropertyContent(parsed, handle, state);
}

void ApplyPropertyContent(const ParsedProperty &parsed, const SdfPropertySpecHandle &handle,
                          ApplyState &state) {
    SdfSpec spec = handle.GetSpec();
    const SdfPath path = handle->GetPath();
    const bool isRelationship = (handle->GetSpecType() == SdfSpecTypeRelationship);

    if (handle->IsCustom() != parsed.custom && !state.dryRun) {
        handle->SetCustom(parsed.custom);
    }
    const SdfVariability kindDefault =
        isRelationship ? SdfVariabilityUniform : SdfVariabilityVarying;
    const SdfVariability effective =
        spec.GetField(SdfFieldKeys->Variability).GetWithDefault<SdfVariability>(kindDefault);
    if (parsed.variability != effective && !state.dryRun) {
        if (parsed.variability == kindDefault) {
            spec.ClearField(SdfFieldKeys->Variability);
        } else {
            spec.SetField(SdfFieldKeys->Variability, VtValue(parsed.variability));
        }
    }

    // Default value (folded placeholder = leave the existing value alone)
    const VtValue currentDefault = spec.GetField(SdfFieldKeys->Default);
    if (parsed.hasDefault) {
        if (parsed.foldedDefault || parsed.unsupportedValueType) {
            if (currentDefault.IsEmpty()) {
                state.Error(path, "the placeholder has no existing value to keep");
            }
        } else if (currentDefault != parsed.defaultValue && !state.dryRun) {
            spec.SetField(SdfFieldKeys->Default, parsed.defaultValue);
        }
    } else if (!currentDefault.IsEmpty() && !state.dryRun) {
        spec.ClearField(SdfFieldKeys->Default);
    }

    // Connections / relationship targets (field-level, the canonical text
    // representation)
    const TfToken listField =
        isRelationship ? SdfFieldKeys->TargetPaths : SdfFieldKeys->ConnectionPaths;
    const bool parsedHasList = isRelationship ? parsed.hasTargets : parsed.hasConnections;
    const SdfPathListOp &parsedList = isRelationship ? parsed.targets : parsed.connections;
    const VtValue currentList = spec.GetField(listField);
    if (parsedHasList) {
        const bool same = currentList.IsHolding<SdfPathListOp>() &&
                          currentList.UncheckedGet<SdfPathListOp>() == parsedList;
        if (!same && !state.dryRun) {
            spec.SetField(listField, VtValue(parsedList));
        }
    } else if (!currentList.IsEmpty() && !state.dryRun) {
        spec.ClearField(listField);
    }

    // Time samples (folded sample values are taken from the current map)
    const VtValue currentSamplesValue = spec.GetField(SdfFieldKeys->TimeSamples);
    if (parsed.hasTimeSamples) {
        SdfTimeSampleMap finalSamples = parsed.timeSamples;
        if (!parsed.foldedSampleTimes.empty()) {
            const SdfTimeSampleMap currentSamples =
                currentSamplesValue.GetWithDefault<SdfTimeSampleMap>(SdfTimeSampleMap());
            for (double time : parsed.foldedSampleTimes) {
                auto it = currentSamples.find(time);
                if (it == currentSamples.end()) {
                    state.Error(path, TfStringPrintf(
                                          "folded sample at time %g has no existing value", time));
                } else {
                    finalSamples[time] = it->second;
                }
            }
        }
        const bool same = currentSamplesValue.IsHolding<SdfTimeSampleMap>() &&
                          currentSamplesValue.UncheckedGet<SdfTimeSampleMap>() == finalSamples;
        if (!same && !state.dryRun && state.ok) {
            spec.SetField(SdfFieldKeys->TimeSamples, VtValue(finalSamples));
        }
    } else if (!currentSamplesValue.IsEmpty() && !state.dryRun) {
        spec.ClearField(SdfFieldKeys->TimeSamples);
    }

    // Splines are opaque: kept when present in the text, cleared when the
    // statement was deleted; they cannot be created or edited as text yet
    const VtValue currentSpline = spec.GetField(SdfFieldKeys->Spline);
    if (parsed.hasSpline) {
        if (currentSpline.IsEmpty()) {
            state.Error(path, "splines cannot be created from text yet");
        }
    } else if (!currentSpline.IsEmpty() && !state.dryRun) {
        spec.ClearField(SdfFieldKeys->Spline);
    }

    ApplyMetadataToSpec(parsed.metadata, spec, path,
                        isRelationship ? IsRelationshipMetadataField : IsAttributeMetadataField,
                        state);
}

void ApplyProperty(const ParsedProperty &parsed, const SdfPropertySpecHandle &handle,
                   bool allowRename, ApplyState &state) {
    const SdfPath path = handle->GetPath();
    const bool isRelationship = (handle->GetSpecType() == SdfSpecTypeRelationship);
    if (parsed.isRelationship != isRelationship) {
        state.Error(path, "changing between attribute and relationship is not supported in one "
                          "edit — delete the property first");
        return;
    }
    if (parsed.name != handle->GetName()) {
        if (!allowRename) {
            state.Error(path, "internal: unexpected property name mismatch");
            return;
        }
        std::string whyNot;
        if (!handle->CanSetName(parsed.name, &whyNot)) {
            state.Error(path, "cannot rename to '" + parsed.name + "': " + whyNot);
            return;
        }
        if (!state.dryRun) {
            handle->SetName(parsed.name);
        }
    }
    // Attribute type change: remove + recreate
    if (!isRelationship) {
        const SdfAttributeSpecHandle attribute = SdfSpecStatic_cast<SdfAttributeSpecHandle>(handle);
        const std::string currentTypeName =
            UsdaGetSerializationName(attribute->GetTypeName()).GetString();
        if (parsed.typeName != currentTypeName) {
            if (parsed.foldedDefault || !parsed.foldedSampleTimes.empty()) {
                state.Error(path, "cannot retype a property whose values are folded");
                return;
            }
            ValidateNewProperty(parsed, path.GetParentPath(), state);
            if (!state.dryRun && state.ok) {
                const SdfPrimSpecHandle owner =
                    handle->GetLayer()->GetPrimAtPath(handle->GetPath().GetPrimPath());
                owner->RemoveProperty(handle);
                CreatePropertyFromParsed(parsed, owner, state);
            }
            return;
        }
    }
    ApplyPropertyContent(parsed, handle, state);
}

void ApplyPrim(const ParsedPrim &parsed, const SdfPrimSpecHandle &spec, bool allowRename,
               ApplyState &state);

void ApplyPrimBody(const ParsedPrim &parsed, const SdfPrimSpecHandle &spec, ApplyState &state) {
    const SdfPath path = spec->GetPath();

    // Reorder statements (writer emits them only when size > 1)
    const std::vector<TfToken> &currentPropertyOrder = spec->GetPropertyOrder();
    const std::vector<TfToken> effectivePropertyOrder =
        currentPropertyOrder.size() > 1 ? currentPropertyOrder : std::vector<TfToken>();
    if (parsed.propertyOrder != effectivePropertyOrder && !state.dryRun) {
        spec->SetPropertyOrder(parsed.propertyOrder);
    }
    const std::vector<TfToken> &currentChildrenOrder = spec->GetNameChildrenOrder();
    const std::vector<TfToken> effectiveChildrenOrder =
        currentChildrenOrder.size() > 1 ? currentChildrenOrder : std::vector<TfToken>();
    if (parsed.nameChildrenOrder != effectiveChildrenOrder && !state.dryRun) {
        spec->SetNameChildrenOrder(parsed.nameChildrenOrder);
    }

    // Properties: create / recurse / remove
    std::set<std::string> parsedPropertyNames;
    for (const ParsedProperty &parsedProperty : parsed.properties) {
        parsedPropertyNames.insert(parsedProperty.name);
        SdfPropertySpecHandle handle = spec->GetProperties().get(TfToken(parsedProperty.name));
        if (handle) {
            const bool isRelationship = (handle->GetSpecType() == SdfSpecTypeRelationship);
            if (parsedProperty.isRelationship != isRelationship) {
                // kind change: remove + recreate
                ValidateNewProperty(parsedProperty, path, state);
                if (!state.dryRun && state.ok) {
                    spec->RemoveProperty(handle);
                    CreatePropertyFromParsed(parsedProperty, spec, state);
                }
            } else {
                ApplyProperty(parsedProperty, handle, /* allowRename = */ false, state);
            }
        } else {
            if (state.dryRun) {
                ValidateNewProperty(parsedProperty, path, state);
            } else {
                CreatePropertyFromParsed(parsedProperty, spec, state);
            }
        }
    }
    std::vector<SdfPropertySpecHandle> propertiesToRemove;
    for (const SdfPropertySpecHandle &handle : spec->GetProperties()) {
        if (!parsedPropertyNames.count(handle->GetName())) {
            propertiesToRemove.push_back(handle);
        }
    }
    if (!state.dryRun) {
        for (const SdfPropertySpecHandle &handle : propertiesToRemove) {
            spec->RemoveProperty(handle);
        }
    }

    // Children prims: create / recurse / remove
    std::set<std::string> parsedChildNames;
    for (const auto &parsedChild : parsed.children) {
        parsedChildNames.insert(parsedChild->name);
        SdfPrimSpecHandle childSpec = spec->GetNameChildren().get(TfToken(parsedChild->name));
        if (childSpec) {
            ApplyPrim(*parsedChild, childSpec, /* allowRename = */ false, state);
        } else {
            if (!SdfPath::IsValidIdentifier(parsedChild->name)) {
                state.Error(path, "'" + parsedChild->name + "' is not a valid prim name");
                continue;
            }
            if (!state.dryRun) {
                SdfPrimSpecHandle created =
                    SdfPrimSpec::New(spec, parsedChild->name, parsedChild->specifier,
                                     parsedChild->hasTypeName ? parsedChild->typeName : "");
                if (!created) {
                    state.Error(path, "could not create prim '" + parsedChild->name + "'");
                    continue;
                }
                ApplyPrim(*parsedChild, created, /* allowRename = */ false, state);
            } else {
                // dry-validate the new subtree without specs to compare to
                for (const ParsedProperty &newProperty : parsedChild->properties) {
                    ValidateNewProperty(newProperty, path, state);
                }
            }
        }
    }
    std::vector<SdfPrimSpecHandle> childrenToRemove;
    for (const SdfPrimSpecHandle &childSpec : spec->GetNameChildren()) {
        if (!parsedChildNames.count(childSpec->GetName())) {
            childrenToRemove.push_back(childSpec);
        }
    }
    if (!state.dryRun) {
        for (const SdfPrimSpecHandle &childSpec : childrenToRemove) {
            spec->RemoveNameChild(childSpec);
        }
    }

    // Match the stored child order to the order the prims appear in the text.
    // SdfPrimSpec::New always appends, so a newly written prim would otherwise
    // land after its siblings instead of where it was typed; this also lets a
    // text edit reorder existing prims.
    if (!state.dryRun) {
        const auto nameChildren = spec->GetNameChildren();
        std::vector<TfToken> desiredOrder;
        desiredOrder.reserve(nameChildren.size());
        std::set<TfToken> placed;
        for (const auto &parsedChild : parsed.children) {
            const TfToken childName(parsedChild->name);
            if (nameChildren.get(childName) && placed.insert(childName).second) {
                desiredOrder.push_back(childName);
            }
        }
        // Keep any existing child the text did not mention (e.g. on error) so the
        // order list always stays a permutation of the actual children.
        for (const SdfPrimSpecHandle &childSpec : nameChildren) {
            const TfToken childName = childSpec->GetNameToken();
            if (placed.insert(childName).second) {
                desiredOrder.push_back(childName);
            }
        }
        const std::vector<TfToken> currentOrder =
            spec->GetFieldAs<std::vector<TfToken>>(SdfChildrenKeys->PrimChildren);
        if (currentOrder != desiredOrder) {
            spec->SetField(SdfChildrenKeys->PrimChildren, VtValue(desiredOrder));
        }
    }

    // Variant sets: edit inside, add and remove variants/sets
    const SdfVariantSetsProxy variantSets = spec->GetVariantSets();
    std::set<std::string> parsedSetNames;
    for (const ParsedVariantSet &parsedSet : parsed.variantSets) {
        parsedSetNames.insert(parsedSet.name);
        SdfVariantSetSpecHandle setSpec;
        auto setIt = variantSets.find(parsedSet.name);
        if (setIt != variantSets.end()) {
            setSpec = setIt->second;
        } else if (!state.dryRun) {
            setSpec = SdfVariantSetSpec::New(spec, parsedSet.name);
            if (!setSpec) {
                state.Error(path, "could not create variant set '" + parsedSet.name + "'");
                continue;
            }
        }
        std::set<std::string> parsedVariantNames;
        for (const ParsedVariant &parsedVariant : parsedSet.variants) {
            parsedVariantNames.insert(parsedVariant.name);
            SdfVariantSpecHandle variantSpec;
            if (setSpec) {
                for (const SdfVariantSpecHandle &candidate : setSpec->GetVariantList()) {
                    if (candidate->GetName() == parsedVariant.name) {
                        variantSpec = candidate;
                        break;
                    }
                }
            }
            if (!variantSpec) {
                // New variant: validate in dry runs, create in real runs
                for (const ParsedProperty &newProperty : parsedVariant.body->properties) {
                    ValidateNewProperty(newProperty, path, state);
                }
                if (state.dryRun || !state.ok) {
                    continue;
                }
                variantSpec = SdfVariantSpec::New(setSpec, parsedVariant.name);
                if (!variantSpec) {
                    state.Error(path, "could not create variant '" + parsedVariant.name + "'");
                    continue;
                }
            }
            const SdfPrimSpecHandle variantPrim = variantSpec->GetPrimSpec();
            ApplyMetadataToSpec(parsedVariant.body->metadata, variantPrim.GetSpec(),
                                variantPrim->GetPath(), IsPrimMetadataField, state);
            ApplyPrimBody(*parsedVariant.body, variantPrim, state);
        }
        if (setSpec && !state.dryRun) {
            std::vector<SdfVariantSpecHandle> variantsToRemove;
            for (const SdfVariantSpecHandle &candidate : setSpec->GetVariantList()) {
                if (!parsedVariantNames.count(candidate->GetName())) {
                    variantsToRemove.push_back(candidate);
                }
            }
            for (const SdfVariantSpecHandle &candidate : variantsToRemove) {
                setSpec->RemoveVariant(candidate);
            }
        }
    }
    std::vector<std::string> setsToRemove;
    for (const auto &nameAndSet : variantSets) {
        if (!nameAndSet.second->GetVariantList().empty() && !parsedSetNames.count(nameAndSet.first)) {
            setsToRemove.push_back(nameAndSet.first);
        }
    }
    if (!state.dryRun) {
        for (const std::string &setName : setsToRemove) {
            spec->RemoveVariantSet(setName);
        }
    }
}

void ApplyPrim(const ParsedPrim &parsed, const SdfPrimSpecHandle &spec, bool allowRename,
               ApplyState &state) {
    const SdfPath path = spec->GetPath();

    if (parsed.name != spec->GetName()) {
        if (!allowRename) {
            state.Error(path, "internal: unexpected prim name mismatch");
            return;
        }
        std::string whyNot;
        if (!spec->CanSetName(parsed.name, &whyNot)) {
            state.Error(path, "cannot rename to '" + parsed.name + "': " + whyNot);
            return;
        }
        if (!state.dryRun) {
            spec->SetName(parsed.name);
        }
    }
    if (parsed.hasSpecifier && parsed.specifier != spec->GetSpecifier() && !state.dryRun) {
        spec->SetSpecifier(parsed.specifier);
    }
    // Type name follows the writer's preamble logic
    SdfSpec rawSpec = spec.GetSpec();
    const VtValue currentTypeName = rawSpec.GetField(SdfFieldKeys->TypeName);
    if (parsed.hasTypeName) {
        const VtValue desired = VtValue(TfToken(parsed.typeName));
        if (currentTypeName != desired && !state.dryRun) {
            spec->SetTypeName(parsed.typeName);
        }
    } else if (!currentTypeName.IsEmpty() && !state.dryRun) {
        rawSpec.ClearField(SdfFieldKeys->TypeName);
    }

    ApplyMetadataToSpec(parsed.metadata, rawSpec, path, IsPrimMetadataField, state);
    ApplyPrimBody(parsed, spec, state);
}

} // namespace

bool ApplyParsedPrimToSpec(const ParsedPrim &parsed, const SdfPrimSpecHandle &spec, bool dryRun,
                           bool allowRename, std::vector<std::string> *errors) {
    if (!spec) {
        if (errors) {
            errors->push_back("no prim spec to apply to");
        }
        return false;
    }
    ApplyState state;
    state.dryRun = dryRun;
    state.errors = errors;
    ApplyPrim(parsed, spec, allowRename, state);
    return state.ok;
}

bool ApplyParsedPropertyToSpec(const ParsedProperty &parsed, const SdfPropertySpecHandle &spec,
                               bool dryRun, bool allowRename, std::vector<std::string> *errors) {
    if (!spec) {
        if (errors) {
            errors->push_back("no property spec to apply to");
        }
        return false;
    }
    ApplyState state;
    state.dryRun = dryRun;
    state.errors = errors;
    ApplyProperty(parsed, spec, allowRename, state);
    return state.ok;
}

bool ApplyParsedLayerToLayer(const ParsedLayer &parsed, const SdfLayerRefPtr &layer, bool dryRun,
                             std::vector<std::string> *errors) {
    ApplyState state;
    state.dryRun = dryRun;
    state.errors = errors;

    const SdfPrimSpecHandle pseudoRoot = layer->GetPseudoRoot();
    ApplyMetadataToSpec(parsed.metadata, pseudoRoot.GetSpec(), SdfPath::AbsoluteRootPath(),
                        IsLayerMetadataField, state, /* isLayer = */ true);

    const std::vector<TfToken> &rootOrder = layer->GetRootPrimOrder();
    const std::vector<TfToken> effectiveOrder =
        rootOrder.size() > 1 ? rootOrder : std::vector<TfToken>();
    if (parsed.rootPrimOrder != effectiveOrder && !state.dryRun) {
        layer->SetRootPrimOrder(parsed.rootPrimOrder);
    }

    std::set<std::string> parsedNames;
    for (const auto &parsedPrim : parsed.rootPrims) {
        parsedNames.insert(parsedPrim->name);
        SdfPrimSpecHandle spec = layer->GetRootPrims().get(TfToken(parsedPrim->name));
        if (spec) {
            ApplyPrim(*parsedPrim, spec, /* allowRename = */ false, state);
        } else {
            if (!SdfPath::IsValidIdentifier(parsedPrim->name)) {
                state.Error(SdfPath::AbsoluteRootPath(),
                            "'" + parsedPrim->name + "' is not a valid prim name");
                continue;
            }
            if (!state.dryRun) {
                SdfPrimSpecHandle created =
                    SdfPrimSpec::New(layer, parsedPrim->name, parsedPrim->specifier,
                                     parsedPrim->hasTypeName ? parsedPrim->typeName : "");
                if (!created) {
                    state.Error(SdfPath::AbsoluteRootPath(),
                                "could not create prim '" + parsedPrim->name + "'");
                    continue;
                }
                ApplyPrim(*parsedPrim, created, false, state);
            } else {
                for (const ParsedProperty &newProperty : parsedPrim->properties) {
                    ValidateNewProperty(newProperty, SdfPath::AbsoluteRootPath(), state);
                }
            }
        }
    }
    std::vector<SdfPrimSpecHandle> primsToRemove;
    for (const SdfPrimSpecHandle &spec : layer->GetRootPrims()) {
        if (!parsedNames.count(spec->GetName())) {
            primsToRemove.push_back(spec);
        }
    }
    if (!state.dryRun) {
        for (const SdfPrimSpecHandle &spec : primsToRemove) {
            layer->RemoveRootPrim(spec);
        }
    }
    return state.ok;
}
