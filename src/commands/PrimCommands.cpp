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
        : primSpec(), layer(layer), primName(std::move(primName)) {}

    // Create a child prim
    PrimNew(SdfPrimSpecHandle primSpec, std::string primName)
        : // TODO: check std::move does really moves avoid a copy
          primSpec(std::move(primSpec)), layer(), primName(std::move(primName)) {}

    ~PrimNew() override {}

    bool doIt() override {
        if (!layer && !primSpec)
            return false;
        if (layer) {
            auto prim = SdfPrimSpec::New(layer, primName, SdfSpecifier::SdfSpecifierDef);
            layer->InsertRootPrim(prim);
            return true;
        } else {
            SdfPrimSpec::New(primSpec, primName, SdfSpecifier::SdfSpecifierDef);
            return true;
        }
    }
    SdfPrimSpecHandle primSpec;
    SdfLayerRefPtr layer;
    std::string primName;
};

struct PrimRemove : public Command {

    PrimRemove(SdfLayerRefPtr layer, SdfPrimSpecHandle primSpec)
        : primSpec(std::move(primSpec)), layer(std::move(layer)) {}

    ~PrimRemove() override {}

    bool doIt() override {
        if (!primSpec)
            return false;
        if (primSpec->GetNameParent()) {
            primSpec->GetNameParent()->RemoveNameChild(primSpec);
            return true;
        } else {
            layer->RemoveRootPrim(primSpec);
            return true;
        }
    }

    SdfLayerRefPtr layer;
    SdfPrimSpecHandle primSpec;
};

struct PrimChangeName : public Command {

    PrimChangeName(SdfPrimSpecHandle primSpec, std::string newName)
        : primSpec(std::move(primSpec)), newName(std::move(newName)) {}

    ~PrimChangeName() override {}

    bool doIt() override {
        primSpec->SetName(newName);
        return true;
    }

    SdfPrimSpecHandle primSpec;
    std::string newName;
};

struct PrimChangeSpecifier : public Command {
    PrimChangeSpecifier(SdfPrimSpecHandle primSpec, SdfSpecifier newSpecifier)
        : primSpec(std::move(primSpec)), newSpecifier(std::move(newSpecifier)) {}

    ~PrimChangeSpecifier() override {}

    bool doIt() override {
        if (primSpec) {
            primSpec->SetSpecifier(newSpecifier);
            return true;
        }
        return false;
    }

    SdfPrimSpecHandle primSpec;
    SdfSpecifier newSpecifier;
};

struct PrimAddReference : public Command {
    PrimAddReference(SdfPrimSpecHandle primSpec, std::string reference)
        : primSpec(std::move(primSpec)), reference(std::move(reference)) {}

    ~PrimAddReference() override {}

    bool doIt() override {
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
