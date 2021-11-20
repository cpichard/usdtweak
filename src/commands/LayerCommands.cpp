
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

