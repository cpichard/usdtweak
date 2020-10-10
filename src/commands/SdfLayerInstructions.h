#pragma once
#include <pxr/usd/sdf/abstractData.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/layerStateDelegate.h>

#include <iostream>

PXR_NAMESPACE_USING_DIRECTIVE

struct UndoRedoSetField {
    UndoRedoSetField(SdfLayerHandle layer, const SdfPath& path, const TfToken& fieldName, VtValue newValue, VtValue previousValue )
        : _layer(layer), _path(path), _fieldName(fieldName), _newValue(std::move(newValue)), _previousValue(std::move(previousValue)) {}

    UndoRedoSetField(UndoRedoSetField &&) = default;
    ~UndoRedoSetField() = default;

    void DoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            _layer->GetStateDelegate()->SetField(_path, _fieldName, _newValue);
        }
    }

    void UndoIt() {
        if (_layer && _layer->GetStateDelegate()){
            std::cout << "Undoing Set Field" << _path.GetString() << " " << _fieldName.GetString() << " " << _previousValue << std::endl;
            _layer->GetStateDelegate()->SetField(_path, _fieldName, _previousValue);
        }
    }

    SdfLayerHandle _layer;
    const SdfPath _path;
    const TfToken _fieldName;
    VtValue _newValue;
    VtValue _previousValue;
};


struct UndoRedoSetFieldDictValueByKey {
    UndoRedoSetFieldDictValueByKey(SdfLayerHandle layer, const SdfPath &path, const TfToken& fieldName, const TfToken& keyPath, VtValue value, VtValue previousValue)
        :_layer(layer), _path(path), _fieldName(fieldName), _keyPath(keyPath), _newValue(std::move(value)), _previousValue(previousValue) {}

    UndoRedoSetFieldDictValueByKey(UndoRedoSetFieldDictValueByKey &&) = default;
    ~UndoRedoSetFieldDictValueByKey() = default;

    void DoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            std::cout << "Redo SetFieldDictValueByKey " << _path.GetString() << " " << _fieldName.GetString() << " " << _newValue << std::endl;
            _layer->GetStateDelegate()->SetFieldDictValueByKey(_path, _fieldName, _keyPath, _newValue);
        }
    }

    void UndoIt() {
        if (_layer && _layer->GetStateDelegate()){
            std::cout << "Undo SetFieldDictValueByKey " << _path.GetString() << " " << _fieldName.GetString() << " " << _previousValue << std::endl;
            _layer->GetStateDelegate()->SetFieldDictValueByKey(_path, _fieldName, _keyPath, _previousValue);
        }
    }

    SdfLayerHandle _layer;
    const SdfPath _path;
    const TfToken _fieldName;
    const TfToken _keyPath;
    VtValue _newValue;
    VtValue _previousValue;
};


struct UndoRedoSetTimeSample {
    UndoRedoSetTimeSample(SdfLayerHandle layer, const SdfPath& path, double timeCode, VtValue newValue)
        : _layer(layer), _path(path), _timeCode(timeCode), _newValue(std::move(newValue)) {

        if (_layer && _layer->HasField(path, SdfFieldKeys->TimeSamples)) {
            _layer->QueryTimeSample(_path, _timeCode, &_previousValue);
        }
    }
    ~UndoRedoSetTimeSample() = default;
    UndoRedoSetTimeSample(UndoRedoSetTimeSample &&) = default;

    void DoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            std::cout << "Redo SetTimeSample " << _path.GetString() << " " << _timeCode << " " << _newValue << std::endl;
            _layer->GetStateDelegate()->SetTimeSample(_path, _timeCode, _newValue);
        }
    }

    void UndoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            if (_previousValue != VtValue()) {
                _layer->GetStateDelegate()->SetTimeSample(_path, _timeCode, _previousValue);
            }
            else {
                _layer->GetStateDelegate()->SetField(_path, SdfFieldKeys->TimeSamples, _previousValue);
            }
        }
    }

    SdfLayerHandle _layer;
    const SdfPath _path;
    double _timeCode;
    VtValue _newValue;
    VtValue _previousValue;
};

struct UndoRedoCreateSpec {
    UndoRedoCreateSpec(SdfLayerHandle layer, const SdfPath& path, SdfSpecType specType, bool inert)
        : _layer(layer), _path(path), _specType(specType), _inert(inert) {}

    void DoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            _layer->GetStateDelegate()->CreateSpec(_path, _specType, _inert);
        }
    }

    void UndoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            _layer->GetStateDelegate()->DeleteSpec(_path, _inert);
        }
    }


    SdfLayerHandle _layer;
    const SdfPath _path;
    const SdfSpecType _specType;
    const bool _inert;
};


struct UndoRedoDeleteSpec {

    // Need a structure to copy the old data
    struct _SpecCopier : public SdfAbstractDataSpecVisitor {
        explicit _SpecCopier(SdfAbstractData* dst_);
        bool VisitSpec(const SdfAbstractData& src, const SdfPath& path) override;
        void Done(const SdfAbstractData&) override;

        SdfAbstractData* const dst;
    };


    UndoRedoDeleteSpec(SdfLayerHandle layer, const SdfPath &path, bool inert, SdfAbstractDataPtr layerData);

    void DoIt();
    void UndoIt();

    SdfLayerHandle _layer;
    const SdfPath _path;
    const bool _inert;

    SdfAbstractDataPtr _layerData; // TODO: this might change ? isn't it ? normally it's retrieved from the delegate
    const SdfSpecType _deletedSpecType;
    SdfDataRefPtr _deletedData;
};


struct UndoRedoMoveSpec {

    UndoRedoMoveSpec(SdfLayerHandle layer, const SdfPath &oldPath, const SdfPath &newPath)
    : _layer(layer), _oldPath(oldPath), _newPath(newPath) {}


    void DoIt() {
        if (_layer && _layer->GetStateDelegate()){
            _layer->GetStateDelegate()->MoveSpec(_oldPath, _newPath);
        }

    };
    void UndoIt() {
        if (_layer && _layer->GetStateDelegate()){
            _layer->GetStateDelegate()->MoveSpec(_newPath, _oldPath);
        }
    };

    SdfLayerHandle _layer;
    const SdfPath _oldPath;
    const SdfPath _newPath;
};

template <typename ValueT>
struct UndoRedoPushChild {
    UndoRedoPushChild(SdfLayerHandle layer, const SdfPath& parentPath, const TfToken& fieldName, const ValueT& value)
        : _layer(layer), _parentPath(parentPath), _fieldName(fieldName), _value(value) {}


    void UndoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            _layer->GetStateDelegate()->PopChild(_parentPath, _fieldName, _value);
        }
    }

    void DoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            _layer->GetStateDelegate()->PushChild(_parentPath, _fieldName, _value);
        }
    }

    SdfLayerHandle _layer;
    const SdfPath _parentPath;
    const TfToken _fieldName;
    const ValueT _value;
};

template <typename ValueT>
struct UndoRedoPopChild {
    UndoRedoPopChild(SdfLayerHandle layer, const SdfPath& parentPath, const TfToken& fieldName, const ValueT& value)
        : _layer(layer), _parentPath(parentPath), _fieldName(fieldName), _value(value) {}


    void UndoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            _layer->GetStateDelegate()->PushChild(_parentPath, _fieldName, _value);
        }
    }

    void DoIt() {
        if (_layer && _layer->GetStateDelegate()) {
            _layer->GetStateDelegate()->PopChild(_parentPath, _fieldName, _value);
        }
    }

    SdfLayerHandle _layer;
    const SdfPath _parentPath;
    const TfToken _fieldName;
    const ValueT _value;
};





