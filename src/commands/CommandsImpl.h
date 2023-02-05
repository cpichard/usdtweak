#pragma once
#include <SdfCommandGroup.h>
#include <memory>
#include <pxr/usd/usd/stage.h> // For BeginEdition
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

struct Command {
    virtual ~Command(){};
    virtual bool DoIt() = 0;
    virtual bool UndoIt() { return false; }
};

struct SdfLayerCommand : public Command {
    virtual ~SdfLayerCommand(){};
    virtual bool DoIt() override = 0;
    bool UndoIt() override;
    SdfCommandGroup _undoCommands;
};

// Placeholder command for recorder
struct SdfUndoRedoCommand : public SdfLayerCommand {
    bool DoIt() override;
    bool UndoIt() override;
};

// UsdFunctionCall is a transition command, it internally
// run the function passed to its constructor recording all the sdf event
// and creating a new SdfUndoCommand that will be stored in the stack instead of
// itself
struct UsdFunctionCall : public Command {

    template <typename LayerT> UsdFunctionCall(LayerT layer, std::function<void()> func) : _layer(layer), _func(func) {}

    ~UsdFunctionCall() override {}

    /// Undo the last command in the stack
    bool DoIt() override;
    bool UndoIt() override { return false; }

    SdfLayerHandle _layer;
    std::function<void()> _func;
};
