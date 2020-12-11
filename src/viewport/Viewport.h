#pragma once
///
/// OpenGL/Hydra Viewport and its ImGui Window drawing functions.
/// This will eventually be split in 2 different files as the code
/// has grown too much and doing too many thing
///
#include <map>
#include "Manipulator.h"
#include "CameraEditor.h"
#include "TranslationEditor.h"
#include "MouseHoverEditor.h"
#include "SelectionEditor.h"
#include "Selection.h"
#include "Grid.h"

#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#include <pxr/imaging/glf/simpleLight.h>


class Viewport {
public:
    Viewport(UsdStageRefPtr stage, Selection &);
    ~Viewport();

    // Delete copy
    Viewport(const Viewport &) = delete;
    Viewport & operator = (const Viewport &) = delete;

    /// Render hydra
    void Render();

    /// Update internal data: selection, current renderer
    void Update();

    /// Set the hydra render size
    void SetSize(int width, int height);

    /// Draw the full widget
    void Draw();

    UsdTimeCode GetCurrentTimeCode() const { return _renderparams ? _renderparams->frame : UsdTimeCode::Default(); }
    void SetCurrentTimeCode(const UsdTimeCode &tc) { _renderparams->frame = tc; }

    /// Camera framing
    void FrameSelection(const Selection &);
    void FrameRootPrim();

    // Renderer
    UsdImagingGLEngine *_renderer = nullptr;
    UsdImagingGLRenderParams *_renderparams = nullptr;
    GlfDrawTargetRefPtr _drawTarget;
    bool TestIntersection(GfVec2d clickedPoint, SdfPath &outHitPrimPath, SdfPath &outHitInstancerPath, int &outHitInstanceIndex);
    GfVec2d GetPickingBoundarySize() const;

    // GL Lights
    GlfSimpleLightVector _lights;
    GlfSimpleMaterial _material;
    GfVec4f _ambient;

    // Camera --- TODO: should the additional cameras live on the stage session ??
    GfCamera _currentCamera;

    GfCamera & GetCurrentCamera() { return _currentCamera; }
    const GfCamera & GetCurrentCamera() const { return _currentCamera; }

    std::vector<std::pair<std::string, GfCamera>> _cameras;
    CameraEditor _cameraManipulator;
    CameraEditor & GetCameraManipulator() { return _cameraManipulator; }

    // Still testing how TranslationEditor should interacts with the viewport
    // TODO Test multiple viewport. A gizmo can be seen in multiple viewport
    // but only edited in one

    TranslationEditor _translateManipulator;
    TranslationEditor & GetActiveManipulator() { return _translateManipulator; }

    MouseHoverEditor _mouseHover;


    template <typename ViewportEditorT>
    inline
    Manipulator * GetEditor() { return nullptr; }; // This must be specialized

    /// Should be store the selected camera as
    SdfPath _selectedCameraPath; // => activeCameraPath

    // Position of the mouse in the viewport in normalized unit
    double _mouseX = 0.0;
    double _mouseY = 0.0;
    GfVec2d GetMousePosition() const { return { _mouseX, _mouseY }; }

    std::map<UsdStageRefPtr, UsdImagingGLEngine *> _renderers;

    UsdStageRefPtr _stage;
    UsdStageRefPtr GetCurrentStage() { return _stage; }
    void SetCurrentStage(UsdStageRefPtr stage) { _stage = stage; }


    Selection & GetSelection() { return _selection; }
    SelectionHash _lastSelectionHash = 0;
    SelectionEditor _selectionManipulator;
    SelectionEditor & GetSelectionManipulator() {return _selectionManipulator;}

    /// Handle events is implemented as a finite state machine using ViewportEditor
    void HandleEvents();

private:
    Manipulator * _currentEditingState;
    Selection &_selection;
    GLuint _textureId = 0;
    Grid    _grid;
};


template <>
inline
Manipulator * Viewport::GetEditor<TranslationEditor>() { return &_translateManipulator; }

template <>
inline
Manipulator * Viewport::GetEditor<MouseHoverEditor>() { return &_mouseHover; }

template <>
inline
Manipulator * Viewport::GetEditor<CameraEditor>() { return &_cameraManipulator; }

template <>
inline
Manipulator * Viewport::GetEditor<SelectionEditor>() { return &_selectionManipulator; }




