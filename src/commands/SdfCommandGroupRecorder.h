#pragma once

#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/abstractData.h>
#include "SdfCommandGroup.h"

PXR_NAMESPACE_USING_DIRECTIVE

///
/// Scoped sdf commands recorder
///

class SdfCommandGroupRecorder final {
public:

    /// RAII object for recording Sdf "instructions" on one or multiple layers
    SdfCommandGroupRecorder(SdfCommandGroup &undoCommands, SdfLayerRefPtr layer);
    SdfCommandGroupRecorder(SdfCommandGroup &undoCommands, SdfLayerHandleVector layers);
    ~SdfCommandGroupRecorder();

private:
    void SetUndoStateDelegates();
    void UnsetUndoStateDelegates();
    
    // The _undoCommands is used to make sure we don't change the dele
    SdfCommandGroup &_undoCommands;

    // We keep the _previousDelegates of the _layers to restore them when the object is destroyed
    SdfLayerHandleVector _layers;
    SdfLayerStateDelegateBaseRefPtrVector _previousDelegates;
};


/// We might have a SdfRedoRecorder at some point, so alias to a base type
//using SdfCommandRecorder = SdfCommandGroupRecorder;
