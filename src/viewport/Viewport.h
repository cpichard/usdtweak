#pragma once
///
/// OpenGL/Hydra Viewport and its ImGui Window drawing functions.
/// This will eventually be split in 2 different files as the code
/// has grown too much and doing too many thing
///
#include <map>
#include "Manipulator.h"
#include "CameraManipulator.h"
#include "PositionManipulator.h"
#include "MouseHoverManipulator.h"
#include "SelectionManipulator.h"
#include "RotationManipulator.h"
#include "Selection.h"
#include "Grid.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#include <pxr/imaging/glf/simpleLight.h>

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

    /// Set the hydra render size
    void SetSize(int width, int height);

    /// Draw the full widget
    void Draw();

    UsdTimeCode GetCurrentTimeCode() const { return _renderparams ? _renderparams->frame : UsdTimeCode::Default(); }
    void SetCurrentTimeCode(const UsdTimeCode &tc) { if (_renderparams) _renderparams->frame = tc; }

    /// Camera framing
    void FrameSelection(const Selection &);
    void FrameRootPrim();

    // Renderer
    UsdImagingGLEngine *_renderer = nullptr;
    UsdImagingGLRenderParams *_renderparams = nullptr;
    GlfDrawTargetRefPtr _drawTarget;
    bool TestIntersection(GfVec2d clickedPoint, SdfPath &outHitPrimPath, SdfPath &outHitInstancerPath, int &outHitInstanceIndex);
    GfVec2d GetPickingBoundarySize() const;

    double ComputeScaleFactor(const GfVec3d &objectPosition, double multiplier = 1.0) const;

    // GL Lights
    GlfSimpleLightVector _lights;
    GlfSimpleMaterial _material;
    GfVec4f _ambient;

    // Camera --- TODO: should the additional cameras live on the stage session ??
    GfCamera _currentCamera;
    GfCamera &GetCurrentCamera() { return _currentCamera; }
    const GfCamera &GetCurrentCamera() const { return _currentCamera; }
    // std::vector<std::pair<std::string, GfCamera>> _cameras;

    CameraManipulator _cameraManipulator;
    CameraManipulator &GetCameraManipulator() { return _cameraManipulator; }

    PositionManipulator _positionManipulator;
    RotationManipulator _rotationManipulator;
    MouseHoverManipulator _mouseHover;

    /// All the manipulators are currently stored in this class, this might change, but right now
    /// GetManipulator is the function that will return the official manipulator based on its type ManipulatorT
    template <typename ManipulatorT> inline Manipulator *GetManipulator();

    Manipulator *_activeManipulator;
    Manipulator &GetActiveManipulator() { return *_activeManipulator; }

    // The chosen manipulator is the one selected in the toolbar, Translate/Rotate/Scale/Select ...
    // Manipulator *_chosenManipulator;
    template <typename ManipulatorT> inline void ChooseManipulator() { _activeManipulator = GetManipulator<ManipulatorT>(); };
    template <typename ManipulatorT> inline bool IsChosenManipulator() {
        return _activeManipulator == GetManipulator<ManipulatorT>();
    };

    /// Should be store the selected camera as
    SdfPath _selectedCameraPath; // => activeCameraPath

    // Position of the mouse in the viewport in normalized unit
    // This is computed in HandleEvents 
    GfVec2d _mousePosition;
    GfVec2d GetMousePosition() const { return _mousePosition; }

    std::map<UsdStageRefPtr, UsdImagingGLEngine *> _renderers;

    UsdStageRefPtr _stage;
    UsdStageRefPtr GetCurrentStage() { return _stage; }
    void SetCurrentStage(UsdStageRefPtr stage) { _stage = stage; }

    Selection &GetSelection() { return _selection; }

    SelectionManipulator &GetSelectionManipulator() { return _selectionManipulator; }

    /// Handle events is implemented as a finite state machine.
    /// The state are simply the current manipulator used.
    void HandleManipulationEvents();

    void DrawManipulatorToolbox(const struct ImVec2 &origin);

  private:
    Manipulator *_currentEditingState;
    SelectionManipulator _selectionManipulator; // TODO: rename to SelectionManipulator
    Selection &_selection;
    GLuint _textureId = 0;
    Grid _grid;
    SelectionHash _lastSelectionHash = 0;
    GfVec2i _viewportSize;
};

template <> inline Manipulator *Viewport::GetManipulator<PositionManipulator>() { return &_positionManipulator; }
template <> inline Manipulator *Viewport::GetManipulator<RotationManipulator>() { return &_rotationManipulator; }
template <> inline Manipulator *Viewport::GetManipulator<MouseHoverManipulator>() { return &_mouseHover; }
template <> inline Manipulator *Viewport::GetManipulator<CameraManipulator>() { return &_cameraManipulator; }
template <> inline Manipulator *Viewport::GetManipulator<SelectionManipulator>() { return &_selectionManipulator; }
