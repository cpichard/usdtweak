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

    bool UndoIt() override {return false;}
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