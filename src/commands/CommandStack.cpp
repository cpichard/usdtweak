#include "CommandStack.h"
#include "SdfCommandGroupRecorder.h"
#include "UsdSceneLock.h"

CommandStack *CommandStack::instance = nullptr;

CommandStack &CommandStack::GetInstance() {
    if (!instance) {
        instance = new CommandStack();
    }
    return *instance;
}

CommandStack::CommandStack() {}
CommandStack::~CommandStack() {
    if (instance) {
        delete instance;
    }
}

void CommandStack::ExecuteCommands() {
    // Take the command out of the slot first (under the slot mutex, never
    // while holding the scene lock — a worker queueing the next command must
    // not be able to deadlock against a writer).
    Command *cmd = nullptr;
    {
        std::lock_guard<std::mutex> lock(_lastCmdMutex);
        cmd = lastCmd;
        lastCmd = nullptr;
    }
    if (!cmd)
        return;

    // Commands are THE write path: exclusive scene access while one runs, so
    // background readers (Twiki dispatcher, UtqlEngine, search index) never
    // observe a half-applied edit (USD: parallel reads, single-thread write).
    ScopedSceneWrite sceneWrite;
    if (cmd->DoIt()) {
        _PushCommand(cmd);
    } else {
        delete cmd;
    }
}

void CommandStack::_PushCommand(Command *cmd) {
    if (undoStackPos != undoStack.size()) {
        undoStack.resize(undoStackPos);
    }
    undoStack.emplace_back(std::move(cmd));
    undoStackPos++;
}

struct UndoCommand : public Command {

    UndoCommand() {}
    ~UndoCommand() override {}

    /// Undo the last command in the stack
    bool DoIt() override ;
    bool UndoIt() override { return false; }
};

struct RedoCommand : public Command {

    RedoCommand() {}
    ~RedoCommand() override {}

    /// Undo the last command in the stack
    bool DoIt() override;
    bool UndoIt() override { return false; }
};


struct ClearUndoRedoCommand : public Command {

    ClearUndoRedoCommand() {}
    ~ClearUndoRedoCommand() override {}

    /// Undo the last command in the stack
    bool DoIt() override;
    bool UndoIt() override { return false; }
};

// EditorUndo Command
bool UndoCommand::DoIt() {
    CommandStack &commandStack = CommandStack::GetInstance();
    // TODO : move into stacK ??
    if (commandStack.undoStackPos > 0) {
        commandStack.undoStackPos--;
        commandStack.undoStack[commandStack.undoStackPos]->UndoIt();
    }
    return false; // Should never be stored in the stack
}
template void ExecuteAfterDraw<UndoCommand>();

void QueueUndo() { ExecuteAfterDraw<UndoCommand>(); }

/// Undo the last command in the stack
/// Editor Redo command
bool RedoCommand::DoIt() {
    // TODO : move into stacK ??
    CommandStack &commandStack = CommandStack::GetInstance();
    if (commandStack.undoStackPos < commandStack.undoStack.size()) {
        commandStack.undoStack[commandStack.undoStackPos]->DoIt();
        commandStack.undoStackPos++;
    }

    return false; // Should never be stored in the stack
}

template void ExecuteAfterDraw<RedoCommand>();

void QueueRedo() { ExecuteAfterDraw<RedoCommand>(); }

/// Generic command that runs an arbitrary callback once on the UI thread.
/// Not stored in the undo stack.
struct EditorRunCallback : public Command {
    std::function<void()> _fn;
    explicit EditorRunCallback(std::function<void()> fn) : _fn(std::move(fn)) {}
    bool DoIt() override {
        if (_fn) _fn();
        return false;  // don't push onto the undo stack
    }
    bool UndoIt() override { return false; }
};
template void ExecuteAfterDraw<EditorRunCallback>(std::function<void()>);

void QueueOnUIThread(std::function<void()> fn) {
    ExecuteAfterDraw<EditorRunCallback>(std::move(fn));
}

/// Undo the last command in the stack
bool ClearUndoRedoCommand::DoIt() {
    CommandStack &commandStack = CommandStack::GetInstance();
    commandStack.undoStackPos = 0;
    commandStack.undoStack.clear();
    // Drop any command a worker queued since this one was extracted.
    Command *pending = nullptr;
    {
        std::lock_guard<std::mutex> lock(commandStack._lastCmdMutex);
        pending = commandStack.lastCmd;
        commandStack.lastCmd = nullptr;
    }
    delete pending;
    return false; // Should never be stored in the stack
}
template void ExecuteAfterDraw<ClearUndoRedoCommand>();

/// Undo the last command in the stack
bool UsdFunctionCall::DoIt() {
    CommandStack &commandStack = CommandStack::GetInstance();
    SdfUndoRedoCommand *command = new SdfUndoRedoCommand();
    {
        SdfCommandGroupRecorder recorder(command->_undoCommands, _layer);
        _func();
    }
    // Push this SdfUndoRedoCommand command on the stack
    commandStack._PushCommand(command);

    // We don't want to push UsdFunctionCall, as we already pushed
    // a SdfUndoRedoCommand
    return false;
}

template <> UsdFunctionCall::UsdFunctionCall(UsdStageRefPtr stage, std::function<void()> func) : _layer(), _func(func) {
    if (stage) { // I am assuming the edits always go in the edit target layer
        _layer = stage->GetEditTarget().GetLayer();
    }
}

template void ExecuteAfterDraw<UsdFunctionCall>(SdfLayerRefPtr layer, std::function<void()> func);
template void ExecuteAfterDraw<UsdFunctionCall>(SdfLayerHandle layer, std::function<void()> func);
template void ExecuteAfterDraw<UsdFunctionCall>(UsdStageRefPtr stage, std::function<void()> func);

/// Plan first (read-only, returns the destination layers), then record the
/// edits `apply` makes on those layers as one undoable SdfUndoRedoCommand.
bool MultiLayerFunctionCall::DoIt() {
    SdfLayerHandleVector layers = _prepare ? _prepare() : SdfLayerHandleVector();
    SdfUndoRedoCommand *command = new SdfUndoRedoCommand();
    {
        SdfCommandGroupRecorder recorder(command->_undoCommands, layers);
        if (_apply)
            _apply();
    }
    if (command->_undoCommands.IsEmpty()) {
        // Nothing was authored (dry run / all rows skipped): no undo entry.
        delete command;
    } else {
        CommandStack::GetInstance()._PushCommand(command);
    }
    return false; // the SdfUndoRedoCommand was pushed instead of this command
}

template void ExecuteAfterDraw<MultiLayerFunctionCall>(std::function<SdfLayerHandleVector()> prepare,
                                                       std::function<void()> apply);


// Should go in Commands.cpp ???
void ExecuteCommands() {
    CommandStack::GetInstance().ExecuteCommands();
}
