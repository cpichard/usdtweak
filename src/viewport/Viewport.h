#pragma once
///
/// OpenGL/Hydra Viewport and its ImGui Window drawing functions.
/// This will eventually be split in 2 different files as the code
/// has grown too much and doing too many thing
///
#include <map>
#include <chrono>
#include "Manipulator.h"
#include "CameraManipulator.h"
#include "PositionManipulator.h"
#include "MouseHoverManipulator.h"
#include "SelectionManipulator.h"
#include "RotationManipulator.h"
#include "ScaleManipulator.h"
#include "Selection.h"
#include "Grid.h"
#include "ViewportCameras.h"
#include <pxr/imaging/glf/drawTarget.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>

#include <ImagingSettings.h>

class Viewport final {
  public:
    Viewport(UsdStageRefPtr stage, Selection &);
    ~Viewport();

    // Delete copy
    Viewport(const Viewport &) = delete;
    Viewport &operator=(const Viewport &) = delete;

    /// Render hydra
    void Render();

    /// Update internal data: selection, current renderer
    void Update();

    /// Draw the full widget
    void Draw();

    /// Returns the time code of this viewport
    UsdTimeCode GetCurrentTimeCode() const { return _imagingSettings.frame; }
    void SetCurrentTimeCode(const UsdTimeCode &tc);

    /// Camera framing
    void FrameSelection(const Selection &);
    void FrameRootPrim();

    /// Return the camera structure used to render the viewport, it can be altered for reframing
    GfCamera &GetEditableCamera();
    const GfCamera &GetCurrentCamera() const;
    
    /// Returns the path of the selected stage camera or SdfPath() if the camera is internal
    const SdfPath &GetSelectedStageCameraPath () { return _cameras.GetStageCameraPath(); }
    
    bool IsEditingStageCamera() const { return _cameras.IsUsingStageCamera(); }
    
    CameraManipulator &GetCameraManipulator() { return _cameraManipulator; }

    // Picking
    bool TestIntersection(GfVec2d clickedPoint, SdfPath &outHitPrimPath, SdfPath &outHitInstancerPath, int &outHitInstanceIndex);
    GfVec2d GetPickingBoundarySize() const;

    // Utility function for compute a scale for the manipulators. It uses the distance between the camera
    // and objectPosition. TODO: remove multiplier, not useful anymore
    double ComputeScaleFactor(const GfVec3d &objectPosition, double multiplier = 1.0) const;

    /// All the manipulators are currently stored in this class, this might change, but right now
    /// GetManipulator is the function that will return the official manipulator based on its type ManipulatorT
    template <typename ManipulatorT> inline Manipulator *GetManipulator();
    Manipulator &GetActiveManipulator() { return *_activeManipulator; }

    // The chosen manipulator is the one selected in the toolbar, Translate/Rotate/Scale/Select ...
    // Manipulator *_chosenManipulator;
    template <typename ManipulatorT> inline void ChooseManipulator() { _activeManipulator = GetManipulator<ManipulatorT>(); };
    template <typename ManipulatorT> inline bool IsChosenManipulator() {
        return _activeManipulator == GetManipulator<ManipulatorT>();
    };

    /// Draw manipulator toolbox, to select translate, rotate, scale
    void DrawManipulatorToolbox(const ImVec2 widgetPosition);
    
    /// Draw toolbar: camera selection, renderer options, viewport options ...
    void DrawToolBar(const ImVec2 widgetPosition);
    
    /// Draw stage selector
    void DrawStageSelector(const ImVec2 widgetPosition);

    // Position of the mouse in the viewport in normalized unit
    // This is computed in HandleEvents

    GfVec2d GetMousePosition() const { return _mousePosition; }

    UsdStageRefPtr GetCurrentStage() { return _stage; }
    const UsdStageRefPtr & GetCurrentStage() const { return _stage; };

    void SetCurrentStage(UsdStageRefPtr stage) { _stage = stage; }

    Selection &GetSelection() { return _selection; }

    /// Handle events is implemented as a finite state machine.
    /// The state are simply the current manipulator used.
    void HandleManipulationEvents();
    void HandleKeyboardShortcut();

    /// Playback controls
    void StartPlayback();
    void StopPlayback();

  private:
    // Viewport ID
    std::string _viewportName;
    
    // Cameras
    ViewportCameras _cameras;

    // Manipulators
    Manipulator *_currentEditingState; // Manipulator currently used by the FSM
    Manipulator *_activeManipulator;   // Manipulator chosen by the user
    CameraManipulator _cameraManipulator;
    PositionManipulator _positionManipulator;
    RotationManipulator _rotationManipulator;
    MouseHoverManipulator _mouseHover;
    ScaleManipulator _scaleManipulator;
    SelectionManipulator _selectionManipulator;

    Selection &_selection;
    SelectionHash _lastSelectionHash = 0;

    // Hydra canvas
    void BeginHydraUI(int width, int height);
    void EndHydraUI();
    GfVec2i _textureSize;
    GfVec2d _mousePosition;
    Grid _grid;

    UsdStageRefPtr _stage;

    // Renderer
    GLuint _textureId = 0;
    std::map<UsdStageRefPtr, UsdImagingGLEngine *> _renderers;
    UsdImagingGLEngine *_renderer = nullptr;
    ImagingSettings _imagingSettings;
    GlfDrawTargetRefPtr _drawTarget;

    // Playback controls
    bool _playing = false;
    std::chrono::time_point<std::chrono::steady_clock> _lastFrameTime;
};

template <> inline Manipulator *Viewport::GetManipulator<PositionManipulator>() { return &_positionManipulator; }
template <> inline Manipulator *Viewport::GetManipulator<RotationManipulator>() { return &_rotationManipulator; }
template <> inline Manipulator *Viewport::GetManipulator<MouseHoverManipulator>() { return &_mouseHover; }
template <> inline Manipulator *Viewport::GetManipulator<CameraManipulator>() { return &_cameraManipulator; }
template <> inline Manipulator *Viewport::GetManipulator<SelectionManipulator>() { return &_selectionManipulator; }
template <> inline Manipulator *Viewport::GetManipulator<ScaleManipulator>() { return &_scaleManipulator; }
