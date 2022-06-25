
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>

PXR_NAMESPACE_USING_DIRECTIVE

struct LayerRemoveSubLayer : public SdfLayerCommand {

    // Removes a sublayer
    LayerRemoveSubLayer(SdfLayerRefPtr layer, std::string subLayerPath)
        : _layer(layer), _subLayerPath(std::move(subLayerPath)) {}

    ~LayerRemoveSubLayer() override {}

    bool DoIt() override {
        if (!_layer)
            return false;
        SdfUndoRecorder recorder(_undoCommands, _layer);
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
        SdfUndoRecorder recorder(_undoCommands, _layer);
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
        for (size_t i = 0; i < layers.size()-1; i++) {
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
        SdfUndoRecorder recorder(_undoCommands, _layer);
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


/* WARNING: this is a brute force and dumb implementation of storing text modification.
 It basically stores the previous and new layer as text in a string. So .... this will eat up the memory
 quite quickly if used intensively.
 But for now it's a quick way to test if text editing is worth in the application.
 */
struct LayerTextEdit : public SdfLayerCommand {
    
    LayerTextEdit(SdfLayerRefPtr layer, std::string newText) : _layer(layer), _newText(newText) {}
    
    ~LayerTextEdit() override {}
    
    bool DoIt() override {
        if(!_layer) return false;
        SdfUndoRecorder recorder(_undoCommands, _layer);
        if(_oldText.empty()) {
            _layer->ExportToString(&_oldText);
        }
        return _layer->ImportFromString(_newText);
        //_layer->SetDirty();
    };
    
    bool UndoIt() override {
        if(!_layer) return false;
        return _layer->ImportFromString(_oldText);
    }

    SdfLayerRefPtr _layer;
    std::string _oldText;
    std::string _newText;
};
template void ExecuteAfterDraw<LayerTextEdit>(SdfLayerRefPtr layer, std::string newText);
