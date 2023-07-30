///
/// This file should be included in Commands.cpp to be compiled with it.
/// IT SHOULD NOT BE COMPILED SEPARATELY
///

///
/// Work in progress, this is not used yet
///
#include "CommandsImpl.h"
#include "Editor.h"
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdUtils/dependencies.h>
#include <string>
#include "WildcardsCompare.h"

#include "SdfUndoRedoRecorder.h"
///
/// Base class for an editor command, contai ns only a pointer of the editor
///
struct EditorCommand : public Command {
    static Editor *_editor;
};
Editor *EditorCommand::_editor = nullptr;

/// The editor commands need a pointer to the data
/// Other client can use this function to pass a handle to their data structure
struct EditorSetDataPointer : public EditorCommand {
    EditorSetDataPointer(Editor *editor) {
        if (!_editor)
            _editor = editor;
    }
    bool DoIt() override { return false; }
};
template void ExecuteAfterDraw<EditorSetDataPointer>(Editor *editor);

struct EditorSetSelection : public EditorCommand {
    EditorSetSelection(UsdStageRefPtr stage, SdfPath path)
    : _stageRefPtr(stage), _path(path) {}
    
    EditorSetSelection(const UsdStageWeakPtr & stage, SdfPath path)
    : _stageRefPtr(stage), _path(path) {}
    
    EditorSetSelection(SdfLayerRefPtr layer, SdfPath path)
    : _layer(layer), _path(path) {}
    
    EditorSetSelection(SdfLayerHandle layer, SdfPath path)
    : _layer(layer), _path(path) {}
    
    ~EditorSetSelection() override {}

    // TODO: wip, we want an "Selection" object to be passed around
    // At the moment it is just the pointer to the current selection held by the editor
    bool DoIt() override {
        if(_editor) {
            auto & selection = _editor->GetSelection();
            if (_layer) {
                _editor->SetCurrentLayer(_layer);
                _editor->SetLayerPathSelection(_path);
            }
            if (_stageRefPtr) {
                _editor->SetCurrentStage(_stageRefPtr);
                _editor->SetStagePathSelection(_path);
            }
        }
        return false;
    }
    UsdStageRefPtr _stageRefPtr;
    SdfLayerRefPtr _layer;
    SdfPath _path;
};
template void ExecuteAfterDraw<EditorSetSelection>(UsdStageWeakPtr, SdfPath);
template void ExecuteAfterDraw<EditorSetSelection>(UsdStageRefPtr, SdfPath);
template void ExecuteAfterDraw<EditorSetSelection>(SdfLayerRefPtr, SdfPath);
template void ExecuteAfterDraw<EditorSetSelection>(SdfLayerHandle, SdfPath);

// TODO use setlayerlocation instead ???
struct EditorSelectAttributePath : public EditorCommand {

    EditorSelectAttributePath(SdfPath attributePath) : _attributePath(attributePath) {}

    ~EditorSelectAttributePath() override {}

    // TODO: wip, we want an "Selection" object to be passed around
    // At the moment it is just the pointer to the current selection held by the editor
    bool DoIt() override {
        if (_editor) {
            _editor->SetLayerPathSelection(_attributePath);
        }
        return true;
    }
    SdfPath _attributePath;
};
template void ExecuteAfterDraw<EditorSelectAttributePath>(SdfPath attributePath);

struct EditorOpenStage : public EditorCommand {

    EditorOpenStage(std::string stagePath) : _stagePath(std::move(stagePath)) {}
    ~EditorOpenStage() override {}

    bool DoIt() override {
        if (_editor) {
            _editor->OpenStage(_stagePath);
        }

        return false;
    }

    std::string _stagePath;
};
template void ExecuteAfterDraw<EditorOpenStage>(std::string stagePath);

struct EditorSetCurrentStage : public EditorCommand {

    EditorSetCurrentStage(SdfLayerHandle layer) : _layer(layer) {}

    bool DoIt() override {
        if (_editor) {
            if (auto stage = _editor->GetStageCache().FindOneMatching(_layer)) {
                _editor->SetCurrentStage(stage);
            }
        }
        return false; // never store this command
    }
    SdfLayerHandle _layer;
};
template void ExecuteAfterDraw<EditorSetCurrentStage>(SdfLayerHandle layer);

// Sets the current stage edit target and change the current layer to the edit target
struct EditorSetEditTarget : public EditorCommand {

    EditorSetEditTarget(UsdStageRefPtr stage, UsdEditTarget editTarget) : _stage(stage), _editTarget(editTarget) {}
    EditorSetEditTarget(UsdStageWeakPtr stage, UsdEditTarget editTarget) : _stage(stage), _editTarget(editTarget) {}
    EditorSetEditTarget(UsdStageRefPtr stage, UsdEditTarget editTarget, SdfPath dstPath)
        : _stage(stage), _editTarget(editTarget), _dstPath(dstPath) {}
    EditorSetEditTarget(UsdStageWeakPtr stage, UsdEditTarget editTarget, SdfPath dstPath)
        : _stage(stage), _editTarget(editTarget), _dstPath(dstPath) {}
    bool DoIt() override {
        if (_editor && _stage) {
            _stage->SetEditTarget(_editTarget);
            if (_editTarget.GetLayer()) {
                _editor->SetCurrentLayer(_editTarget.GetLayer());
                if (_dstPath != SdfPath()) {
                    _editor->SetLayerPathSelection(_dstPath);
                }
            }
        }
        return false; // never store this command
    }
    UsdStageWeakPtr _stage;
    UsdEditTarget _editTarget;
    SdfPath _dstPath;
};
template void ExecuteAfterDraw<EditorSetEditTarget>(UsdStageRefPtr stage, UsdEditTarget editTarget);
template void ExecuteAfterDraw<EditorSetEditTarget>(UsdStageWeakPtr stage, UsdEditTarget editTarget);
template void ExecuteAfterDraw<EditorSetEditTarget>(UsdStageRefPtr stage, UsdEditTarget editTarget, SdfPath srcPath);
template void ExecuteAfterDraw<EditorSetEditTarget>(UsdStageWeakPtr stage, UsdEditTarget editTarget, SdfPath srcPath);

// Shutting down the editor
struct EditorShutdown : public EditorCommand {
    ~EditorShutdown() override {}
    bool DoIt() override {
        // Checking if we have unsaved file, later on we can also check for connections, etc etc
        if (_editor->HasUnsavedWork()) {
            _editor->ConfirmShutdown("You have unsaved work.");
        } else {
            _editor->Shutdown();
        }
        return false;
    }
};
template void ExecuteAfterDraw<EditorShutdown>();


struct EditorSetCurrentLayer : public EditorCommand {

    EditorSetCurrentLayer(SdfLayerHandle layer) : _layer(layer) {}
    ~EditorSetCurrentLayer() override {}

    bool DoIt() override {
        if (_editor && _layer) {
            _editor->SetCurrentLayer(_layer);
        }

        return false;
    }
    SdfLayerRefPtr _layer;
};
template void ExecuteAfterDraw<EditorSetCurrentLayer>(SdfLayerHandle layer);

struct EditorFindOrOpenLayer : public EditorCommand {

    EditorFindOrOpenLayer(std::string layerPath) : _layerPath(std::move(layerPath)) {}
    ~EditorFindOrOpenLayer() override {}

    bool DoIt() override {
        if (_editor) {
            _editor->FindOrOpenLayer(_layerPath);
        }

        return false;
    }
    std::string _layerPath;
};
template void ExecuteAfterDraw<EditorFindOrOpenLayer>(std::string layerPath);

struct EditorSaveLayerAs : public EditorCommand {

    EditorSaveLayerAs(SdfLayerHandle layer) : _layer(layer) {}
    EditorSaveLayerAs(SdfLayerRefPtr layer) : _layer(layer) {}
    ~EditorSaveLayerAs() override {}

    bool DoIt() override {
        if (_editor) {
            _editor->ShowDialogSaveLayerAs(_layer);
        }
        return false;
    }
    SdfLayerRefPtr _layer;
};
template void ExecuteAfterDraw<EditorSaveLayerAs>(SdfLayerHandle layer);
template void ExecuteAfterDraw<EditorSaveLayerAs>(SdfLayerRefPtr layer);

struct EditorSetPreviousLayer : public EditorCommand {

    EditorSetPreviousLayer() {}
    ~EditorSetPreviousLayer() override {}

    bool DoIt() override {
        if (_editor) {
            _editor->SetPreviousLayer();
        }
        return false;
    }
};
template void ExecuteAfterDraw<EditorSetPreviousLayer>();

struct EditorSetNextLayer : public EditorCommand {

    EditorSetNextLayer() {}
    ~EditorSetNextLayer() override {}

    bool DoIt() override {
        if (_editor) {
            _editor->SetNextLayer();
        }
        return false;
    }
};
template void ExecuteAfterDraw<EditorSetNextLayer>();

struct EditorStartPlayback : public EditorCommand {
    EditorStartPlayback() {}
    ~EditorStartPlayback() override {}

    bool DoIt() override {
        if (_editor) {
            _editor->StartPlayback();
        }
        return false;
    }
};
template void ExecuteAfterDraw<EditorStartPlayback>();

struct EditorStopPlayback : public EditorCommand {
    EditorStopPlayback() {}
    ~EditorStopPlayback() override {}
    bool DoIt() override {
        if (_editor) {
            _editor->StopPlayback();
        }
        return false;
    }
};
template void ExecuteAfterDraw<EditorStopPlayback>();

// Launchers, for the moment we don't make the add/remove commands undoable, but they could be in the future
struct EditorRunLauncher : public EditorCommand {
    EditorRunLauncher(const std::string launcherName) : _launcherName(launcherName) {}
    ~EditorRunLauncher() override {}
    bool DoIt() override {
        if (_editor) {
            _editor->RunLauncher(_launcherName);
        }
        return false;
    }
    std::string _launcherName;
};
template void ExecuteAfterDraw<EditorRunLauncher>(const std::string);

struct EditorAddLauncher : public EditorCommand {
    EditorAddLauncher(const std::string launcherName, const std::string commandLine)
        : _launcherName(launcherName), _commandLine(commandLine) {}
    ~EditorAddLauncher() override {}
    bool DoIt() override {
        if (_editor) {
            _editor->AddLauncher(_launcherName, _commandLine);
        }
        return false;
    }
    std::string _launcherName;
    std::string _commandLine;
};
template void ExecuteAfterDraw<EditorAddLauncher>(const std::string, const std::string);

struct EditorRemoveLauncher : public EditorCommand {
    EditorRemoveLauncher(const std::string launcherName) : _launcherName(launcherName) {}
    ~EditorRemoveLauncher() override {}
    bool DoIt() override {
        if (_editor) {
            _editor->RemoveLauncher(_launcherName);
        }
        return false;
    }
    std::string _launcherName;
};
template void ExecuteAfterDraw<EditorRemoveLauncher>(const std::string);

// This will try to find the next matching prim after the selection
struct EditorFindPrim : public EditorCommand {
    EditorFindPrim(const std::string pattern, bool useRegex) : _pattern(pattern) {
        if (useRegex) {
            _matches = [&](const std::string &str) { return FastWildComparePortable(_pattern.c_str(), str.c_str()); };
        } else {
            _matches = [&](const std::string &str) { return str == _pattern; };
        }
    }
    ~EditorFindPrim() override{};

    bool DoIt() override {
        if (_editor) {
            const auto &stage = _editor->GetCurrentStage();
            auto &selection = _editor->GetSelection();
            auto anchor = selection.GetAnchorPrimPath(stage);
            SdfPath found;
            bool selectedFound = false;
            // Traverse the stage and set the new selection
            auto range = UsdPrimRange::Stage(stage, UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate));
            for (auto iter = range.begin(); iter != range.end(); ++iter) {
                if (iter->GetPath() == anchor) {
                    selectedFound = true;
                } else if (_matches(iter->GetName())) {
                    // Store the first matching path in case we don't find the one
                    // after the anchor
                    if (found == SdfPath()) {
                        found = iter->GetPath();
                        // We don't have an anchor, so the first match is the correct one
                        if (anchor == SdfPath())
                            break;
                    }
                    if (selectedFound) {
                        found = iter->GetPath();
                        break;
                    }
                }
            }
            if (found != SdfPath()) {
                selection.SetSelected(stage, found);
            }
        }
        return false;
    }
    std::function<bool(const std::string &)> _matches;
    std::string _pattern;
};
template void ExecuteAfterDraw<EditorFindPrim>(const std::string, bool useRegex);

struct EditorExportUsdz : public EditorCommand {
    EditorExportUsdz(const std::string destination, bool useArKit) : _destination(destination), _useArKit(useArKit) {}
    bool DoIt() override {
        // UsdUtilsCreateNewUsdzPackage is making changes to the scene, we just undo them when it has finished.
        SdfCommandGroup _undoCommands;
        {
            SdfAssetPath stageAssetPath(_editor->GetCurrentStage()->GetRootLayer()->GetRealPath());
            SdfCommandGroupRecorder recorder(_undoCommands, _editor->GetCurrentStage()->GetUsedLayers());
            if (_useArKit) {
                UsdUtilsCreateNewARKitUsdzPackage(stageAssetPath, _destination);
            } else {
                UsdUtilsCreateNewUsdzPackage(stageAssetPath, _destination);
            }
        }
        _undoCommands.UndoIt();
        return false; // Don't push this command on the undo/redo stack
    }
    
    std::string _destination;
    bool _useArKit;
};
template void ExecuteAfterDraw<EditorExportUsdz>(const std::string, bool);


struct EditorExportFlattenedStage : public EditorCommand {
    EditorExportFlattenedStage(const std::string destination) : _destination(destination) {}
    bool DoIt() override {
        _editor->GetCurrentStage()->Flatten()->Export(_destination);
        return false;
    }
    std::string _destination;
};
template void ExecuteAfterDraw<EditorExportFlattenedStage>(const std::string);
