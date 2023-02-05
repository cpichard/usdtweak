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

    ///
    SdfCommandGroupRecorder(SdfCommandGroup &undoCommands, SdfLayerRefPtr layer);
    ~SdfCommandGroupRecorder();

private:
    SdfCommandGroup &_undoCommands;
    SdfLayerRefPtr _layer;
    SdfLayerStateDelegateBaseRefPtr _previousDelegate;
};


/// We might have a SdfRedoRecorder at some point, so alias to a base type
//using SdfCommandRecorder = SdfCommandGroupRecorder;
