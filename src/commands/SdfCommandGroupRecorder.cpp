#include <iostream>
#include "SdfCommandGroupRecorder.h"
#include "UndoLayerStateDelegate.h"

SdfCommandGroupRecorder::SdfCommandGroupRecorder(SdfCommandGroup &undoCommands, SdfLayerRefPtr layer)
: _undoCommands(undoCommands), _layers({layer}) {
    SetUndoStateDelegates();
}

SdfCommandGroupRecorder::SdfCommandGroupRecorder(SdfCommandGroup &undoCommands, SdfLayerHandleVector layers)
: _undoCommands(undoCommands), _layers(layers) {
    SetUndoStateDelegates();
}

SdfCommandGroupRecorder::~SdfCommandGroupRecorder() {
    UnsetUndoStateDelegates();
}


void SdfCommandGroupRecorder::SetUndoStateDelegates() {
    if (_undoCommands.IsEmpty()) {
        auto stateDelegate = UndoRedoLayerStateDelegate::New(_undoCommands);
        for (const auto &layer : _layers) {
            if (layer) {
                _previousDelegates.push_back(layer->GetStateDelegate());
                layer->SetStateDelegate(stateDelegate);
            } else {
                _previousDelegates.push_back({});
            }
        }
    }
}


void SdfCommandGroupRecorder::UnsetUndoStateDelegates() {
    if (!_layers.empty() && !_previousDelegates.empty()) {
        auto previousDelegateIt = _previousDelegates.begin();
        for (const auto &layer : _layers) {
            if (layer) {
                layer->SetStateDelegate(*previousDelegateIt);
            }
            previousDelegateIt++;
        }
    }
}
