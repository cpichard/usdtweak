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

// The multi-layer sibling of UsdFunctionCall, for edits whose destination
// layers are only known after a planning step (e.g. a UTQL UPDATE spanning
// several layers): `prepare` runs first (read-only) and returns the layers to
// record, then `apply` runs with undo recording active on all of them. The
// recorded edits are stored as a single SdfUndoRedoCommand; nothing is pushed
// when `apply` makes no change (a dry run costs no undo entry).
struct MultiLayerFunctionCall : public Command {
    MultiLayerFunctionCall(std::function<SdfLayerHandleVector()> prepare, std::function<void()> apply)
        : _prepare(std::move(prepare)), _apply(std::move(apply)) {}
    ~MultiLayerFunctionCall() override {}

    bool DoIt() override;
    bool UndoIt() override { return false; }

    std::function<SdfLayerHandleVector()> _prepare;
    std::function<void()> _apply;
};
