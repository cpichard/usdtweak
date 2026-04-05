
#include <pxr/usd/sdf/copyUtils.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>
#include <pxr/usd/sdf/variantSpec.h>
#ifdef ENABLE_VALIDATION_FIXERS
#include <pxr/usdValidation/usdValidation/context.h>
#include <pxr/usdValidation/usdValidation/registry.h>
#include <pxr/usdValidation/usdValidation/validator.h>
#endif
#include "CommandsImpl.h"
#include "SdfUndoRedoRecorder.h"

PXR_NAMESPACE_USING_DIRECTIVE

struct LayerRemoveSubLayer : public SdfLayerCommand {

    // Removes a sublayer
    LayerRemoveSubLayer(SdfLayerRefPtr layer, std::string subLayerPath) : _layer(layer), _subLayerPath(std::move(subLayerPath)) {}

    ~LayerRemoveSubLayer() override {}

    bool DoIt() override {
        if (!_layer)
            return false;
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        for (int i = 0; i < _layer->GetNumSubLayerPaths(); i++) {
            if (_layer->GetSubLayerPaths()[i] == _subLayerPath) {
                _layer->RemoveSubLayerPath(i);
                return true;
            }
        }
        return false;
    }
    SdfLayerRefPtr _layer;
    std::string _subLayerPath;
};
template void ExecuteAfterDraw<LayerRemoveSubLayer>(SdfLayerRefPtr layer, std::string subLayerPath);

/// Change layer position in the layer stack, moving up and down
struct LayerMoveSubLayer : public SdfLayerCommand {

    // Removes a sublayer
    LayerMoveSubLayer(SdfLayerRefPtr layer, std::string subLayerPath, bool movingUp)
        : _layer(layer), _subLayerPath(std::move(subLayerPath)), _movingUp(movingUp) {}

    ~LayerMoveSubLayer() override {}

    bool DoIt() override {
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        return _movingUp ? MoveUp() : MoveDown();
    }

    bool MoveUp() {
        if (!_layer)
            return false;
        std::vector<std::string> layers = _layer->GetSubLayerPaths();
        for (size_t i = 1; i < layers.size(); i++) {
            if (layers[i] == _subLayerPath) {
                std::swap(layers[i], layers[i - 1]);
                _layer->SetSubLayerPaths(layers);
                return true;
            }
        }
        return false;
    }

    bool MoveDown() {
        if (!_layer)
            return false;
        std::vector<std::string> layers = _layer->GetSubLayerPaths();
        for (size_t i = 0; i < layers.size() - 1; i++) {
            if (layers[i] == _subLayerPath) {
                std::swap(layers[i], layers[i + 1]);
                _layer->SetSubLayerPaths(layers);
                return true;
            }
        }
        return false;
    }

    SdfLayerRefPtr _layer;
    std::string _subLayerPath;
    bool _movingUp; /// Template instead ?
};
template void ExecuteAfterDraw<LayerMoveSubLayer>(SdfLayerRefPtr layer, std::string subLayerPath, bool movingUp);

/// Rename a sublayer
struct LayerRenameSubLayer : public SdfLayerCommand {

    // Removes a sublayer
    LayerRenameSubLayer(SdfLayerRefPtr layer, std::string oldName, std::string newName)
        : _layer(layer), _oldName(std::move(oldName)), _newName(std::move(newName)) {}

    ~LayerRenameSubLayer() override {}

    bool DoIt() override {
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        if (!_layer)
            return false;
        std::vector<std::string> layers = _layer->GetSubLayerPaths();
        for (size_t i = 0; i < layers.size(); i++) {
            if (layers[i] == _oldName) {
                layers[i] = _newName;
                _layer->SetSubLayerPaths(layers);
                return true;
            }
        }
        return false;
    }

    SdfLayerRefPtr _layer;
    std::string _oldName;
    std::string _newName;
};
template void ExecuteAfterDraw<LayerRenameSubLayer>(SdfLayerRefPtr layer, std::string oldName, std::string newName);

/// Mute and Unmute seem to keep so additional data outside of Sdf, so they need their own commands
struct LayerMute : public Command {
    LayerMute(SdfLayerRefPtr layer) : _layer(layer) {}
    LayerMute(SdfLayerHandle layer) : _layer(layer) {}
    bool DoIt() override {
        if (!_layer)
            return false;
        _layer->SetMuted(true);
        return true;
    };
    bool UndoIt() override {
        if (_layer)
            _layer->SetMuted(false);
        return false;
    }
    SdfLayerRefPtr _layer;
};
template void ExecuteAfterDraw<LayerMute>(SdfLayerRefPtr layer);
template void ExecuteAfterDraw<LayerMute>(SdfLayerHandle layer);

struct LayerUnmute : public Command {
    LayerUnmute(SdfLayerRefPtr layer) : _layer(layer) {}
    LayerUnmute(SdfLayerHandle layer) : _layer(layer) {}
    bool DoIt() override {
        if (!_layer)
            return false;
        _layer->SetMuted(false);
        return true;
    };
    bool UndoIt() override {
        if (_layer)
            _layer->SetMuted(true);
        return false;
    }
    SdfLayerRefPtr _layer;
};
template void ExecuteAfterDraw<LayerUnmute>(SdfLayerRefPtr layer);
template void ExecuteAfterDraw<LayerUnmute>(SdfLayerHandle layer);

// LayerTextEdit applies a full-text replacement to a layer.
// Undo uses the SdfCommandGroupRecorder which captures the individual SDF field
// mutations produced by ImportFromString, so the undo record is proportional to
// what actually changed rather than storing a full copy of the layer text.
struct LayerTextEdit : public SdfLayerCommand {

    LayerTextEdit(SdfLayerRefPtr layer, std::string newText)
        : _layer(layer), _newText(std::move(newText)) {}

    ~LayerTextEdit() override {}

    bool DoIt() override {
        if (!_layer) return false;
        // Record all SDF mutations for undo.  _undoCommands is owned by
        // SdfLayerCommand and replayed in reverse by UndoIt().
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);
        return _layer->ImportFromString(_newText);
    }

    bool UndoIt() override {
        if (!_layer) return false;
        _undoCommands.UndoIt();
        return true;
    }

    SdfLayerRefPtr _layer;
    std::string    _newText; // kept for potential redo
};
template void ExecuteAfterDraw<LayerTextEdit>(SdfLayerRefPtr layer, std::string newText);

// LayerTextEditPreservingArrays performs the same full-text replacement as
// LayerTextEdit but avoids expensive serialization of large-array attributes
// that the user did NOT touch. Those attributes are snapshotted before the
// import and restored via SdfCopySpec afterwards, all inside the same
// SdfCommandGroupRecorder so undo works identically.
struct LayerTextEditPreservingArrays : public SdfLayerCommand {

    LayerTextEditPreservingArrays(SdfLayerRefPtr layer,
                                  std::string newText,
                                  std::vector<SdfPath> attrPathsToRestore)
        : _layer(std::move(layer))
        , _newText(std::move(newText))
        , _attrPathsToRestore(std::move(attrPathsToRestore)) {}

    ~LayerTextEditPreservingArrays() override {}

    bool DoIt() override {
        if (!_layer) return false;
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);

        // Step 1: Snapshot each unmodified large-array attr into an anonymous
        //         layer. SdfCopySpec copies VtValue by COW reference — no
        //         serialization of array bytes.
        auto snapshot = SdfLayer::CreateAnonymous(".usda");
        for (const SdfPath &attrPath : _attrPathsToRestore) {
            if (!_layer->HasSpec(attrPath)) continue;
            _EnsureParentStub(snapshot, attrPath.GetParentPath());
            SdfCopySpec(_layer, attrPath, snapshot, attrPath);
        }

        // Step 2: Full import — fast because large arrays use "= None" dummies.
        if (!_layer->ImportFromString(_newText)) return false;

        // Step 3: Restore each large-array attr from the snapshot.
        //         Skip prims that the user deleted (parent no longer exists).
        for (const SdfPath &attrPath : _attrPathsToRestore) {
            if (!snapshot->HasSpec(attrPath)) continue;
            if (!_layer->HasSpec(attrPath.GetParentPath())) continue;
            SdfCopySpec(snapshot, attrPath, _layer, attrPath);
        }
        return true;
    }

    bool UndoIt() override {
        if (!_layer) return false;
        _undoCommands.UndoIt();
        return true;
    }

    SdfLayerRefPtr       _layer;
    std::string          _newText;
    std::vector<SdfPath> _attrPathsToRestore;

private:
    // Create empty "over" prim stubs in the snapshot layer so SdfCopySpec
    // can place attribute specs at the correct path.
    static void _EnsureParentStub(SdfLayerRefPtr &layer, const SdfPath &path) {
        if (path.IsAbsoluteRootPath() || layer->HasSpec(path)) return;
        _EnsureParentStub(layer, path.GetParentPath());
        SdfPrimSpec::New(layer->GetPrimAtPath(path.GetParentPath()),
                         path.GetName(), SdfSpecifierOver);
    }
};
template void ExecuteAfterDraw<LayerTextEditPreservingArrays>(
    SdfLayerRefPtr layer, std::string newText, std::vector<SdfPath> attrPathsToRestore);

struct LayerCreateOversFromPath : public SdfLayerCommand {

    LayerCreateOversFromPath(SdfLayerRefPtr layer, std::string path) : _layer(layer), _path(std::move(path)) {}
    ~LayerCreateOversFromPath() {}

    bool DoIt() override {
        if (!_layer)
            return false;

        SdfPath path(_path);
        if (path == SdfPath()) {
            return false;
        }
        SdfCommandGroupRecorder recorder(_undoCommands, _layer);

        for (auto &prefix : path.GetPrefixes()) {
            if (!_layer->HasSpec(prefix)) {
                if (prefix.GetParentPath().IsAbsoluteRootPath()) {
                    SdfPrimSpec::New(_layer, prefix.GetName(), SdfSpecifierOver);
                } else if (prefix.IsPrimVariantSelectionPath()) {
                    auto variant = prefix.GetVariantSelection();
                    SdfCreateVariantInLayer(_layer, prefix.GetParentPath(), variant.first, variant.second);
                } else {
                    SdfPrimSpec::New(_layer->GetPrimAtPath(prefix.GetParentPath()), prefix.GetName(), SdfSpecifierOver);
                }
            }
        }
        return true;
    }

    SdfLayerRefPtr _layer;
    std::string _path;
};
template void ExecuteAfterDraw<LayerCreateOversFromPath>(SdfLayerRefPtr layer, std::string path);

#ifdef ENABLE_VALIDATION_FIXERS
struct LayerFixErrors : public SdfLayerCommand {
    LayerFixErrors(UsdEditTarget editTarget, std::vector<UsdValidationError> errors) : _editTarget(editTarget), _errors(errors) {}
    ~LayerFixErrors() {}

    bool DoIt() override {
        if (!_editTarget.GetLayer())
            return false;

        SdfCommandGroupRecorder recorder(_undoCommands, _editTarget.GetLayer());

        for (int i = 0; i < _errors.size(); ++i) {
            const UsdValidationError &error = _errors[i];
            const std::vector<const UsdValidationFixer *> fixers = error.GetFixers();
            if (fixers.size()) {
                // Do we need to apply all the fixer returned here or are they just fixing the same thing ??
                // We might want to add a UI to let the user choose the fixes to apply
                // For now let's use the first fixer
                fixers[0]->ApplyFix(error, _editTarget);
                // TODO: the fixers are saving the fixed layers which is not what we want.
                // Is there a way to avoid this behaviour ?
            }
        }

        return true;
    }

    UsdEditTarget _editTarget;
    std::vector<UsdValidationError> _errors;

};
template void ExecuteAfterDraw<LayerFixErrors>(UsdEditTarget editTarget, std::vector<UsdValidationError> errors);

#endif // ENABLE_VALIDATION_FIXERS
