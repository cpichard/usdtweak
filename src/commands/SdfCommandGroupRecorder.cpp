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
        // One delegate PER layer, all feeding the same SdfCommandGroup.
        // SdfLayerStateDelegateBase keeps a _layer back-pointer (set on attach)
        // and routes the layer's data edits through it, so a single delegate
        // shared across layers applies every edit to the LAST layer attached.
        for (const auto &layer : _layers) {
            if (layer) {
                _previousDelegates.push_back(layer->GetStateDelegate());
                layer->SetStateDelegate(UndoRedoLayerStateDelegate::New(_undoCommands));
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
