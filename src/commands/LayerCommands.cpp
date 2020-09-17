
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/reference.h>

PXR_NAMESPACE_USING_DIRECTIVE

struct LayerInsertSubLayer : public Command {
    LayerInsertSubLayer(SdfLayerRefPtr layer, std::string subLayerPath)
        : _layer(layer), _subLayerPath(std::move(subLayerPath)) {}
    ~LayerInsertSubLayer(){}
    bool DoIt() override {
        if (!_layer)
            return false;
        _layer->InsertSubLayerPath(_subLayerPath);
        return true;
    }

    SdfLayerRefPtr _layer;
    std::string _subLayerPath;
};
template void DispatchCommand<LayerInsertSubLayer>(SdfLayerRefPtr layer, std::string subLayerPath);

struct LayerRemoveSubLayer : public Command {

    // Removes a sublayer
    LayerRemoveSubLayer(SdfLayerRefPtr layer, std::string subLayerPath)
        : _layer(layer), _subLayerPath(std::move(subLayerPath)) {}

    ~LayerRemoveSubLayer() override {}

    bool DoIt() override {
        if (!_layer)
            return false;

        for (size_t i = 0; i < _layer->GetNumSubLayerPaths(); i++) {
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
template void DispatchCommand<LayerRemoveSubLayer>(SdfLayerRefPtr layer, std::string subLayerPath);

/// Change layer position in the layer stack, moivng up and down
struct LayerMoveSubLayer : public Command {

    // Removes a sublayer
    LayerMoveSubLayer(SdfLayerRefPtr layer, std::string subLayerPath, bool movingUp)
        : _layer(layer), _subLayerPath(std::move(subLayerPath)), _movingUp(movingUp) {}

    ~LayerMoveSubLayer() override {}

    bool DoIt() override {
        return _movingUp ? MoveUp() : MoveDown();
    }

    // undo is easy !
    // return _movingUp ? MoveDown() : MoveUp();

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
    bool _movingUp; // This cost a bool, create another command if too costly
};
template void DispatchCommand<LayerMoveSubLayer>(SdfLayerRefPtr layer, std::string subLayerPath, bool movingUp);





