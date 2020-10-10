///
/// This file must be included in Commands.cpp and not compiled separately
///

struct UndoCommand : public Command {

    UndoCommand() {}

    ~UndoCommand() override {}

    /// Undo the last command in the stack
    bool DoIt() override {
        if (undoStackPos > 0) {
            undoStackPos--;
            undoStack[undoStackPos]->UndoIt();
        }

        return false; // Should never be stored in the stack
    }

    bool UndoIt() override { return false; }
};
template void DispatchCommand<UndoCommand>();

struct RedoCommand : public Command {

    RedoCommand() {}

    ~RedoCommand() override {}

    /// Undo the last command in the stack
    bool DoIt() override {
        if (undoStackPos < undoStack.size()) {
            undoStack[undoStackPos]->DoIt();
            undoStackPos++;
        }

        return false; // Should never be stored in the stack
    }

    bool UndoIt() override { return false; }
};
template void DispatchCommand<RedoCommand>();


struct UsdApiFunction : public Command {

    template <typename LayerT>
    UsdApiFunction(LayerT layer, std::function<void()> func)
        : _layer(layer), _func(func)
    {}

    //UsdApiFunction(SdfLayerHandle layer, std::function<void()> func)
    //    : _layer(layer), _func(func)
    //{}

    ~UsdApiFunction() override {}

    /// Undo the last command in the stack
    bool DoIt() override {
        auto command = new SdfUndoRedoCommand();
        {
            SdfUndoRecorder recorder(command->_instructions, _layer);
            _func();
        }
        // Push this command on the stack
        _PushCommand(command);

        return false; // Should never be stored in the stack
    }

    bool UndoIt() override { return false; }

    SdfLayerHandle _layer;
    std::function<void()> _func;
};

template void DispatchCommand<UsdApiFunction>(SdfLayerRefPtr layer, std::function<void()> func);
template void DispatchCommand<UsdApiFunction>(SdfLayerHandle layer, std::function<void()> func);