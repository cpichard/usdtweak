#pragma once
///
/// OpenGL/Hydra Viewport and its ImGui Window drawing functions.
/// This will eventually be split in 2 different files as the code
/// has grown too much and doing too many thing
///
#include <map>
#include "CameraManipulator.h"
#include "TranslateGizmo.h" // Manipulator ??
#include "Selection.h"
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>
#include <pxr/imaging/glf/simpleLight.h>

typedef std::function<void (class Viewport &, Selection &)> HandleEventsFunction;

class Viewport {
public:
    Viewport(UsdStageRefPtr stage);
    ~Viewport();

    // Delete copy constructors
    Viewport(const Viewport &) = delete;
    Viewport & operator = (const Viewport &) = delete;


    void Render(Selection &);
    void Update(Selection &);

    void SetSize(int width, int height);
    void Draw();

    void FrameSelection(Selection & );
    void FrameRootPrim();

    GLuint _textureId = 0;

    // Renderer
    UsdImagingGLEngine *_renderer = nullptr;
    UsdImagingGLRenderParams *_renderparams = nullptr;
    GlfDrawTargetRefPtr _drawTarget;

    // GL Lights
    GlfSimpleLightVector _lights;
    GlfSimpleMaterial _material;
    GfVec4f _ambient;

    // Camera --- TODO: should they live on the stage session ??
    // That would make sense
    GfCamera _currentCamera;
    GfCamera & GetCurrentCamera() { return _currentCamera; }
    std::vector<std::pair<std::string, GfCamera>> _cameras;
    CameraManipulator _cameraManipulator;

    // Here for testing,
    // how a translate gizmo interacts with the viewport
    // Test multiple viewport. A gizmo can be seen in multiple viewport
    // but only edited in one

    TranslateGizmo _translateManipulator;

    /// Should be store the selected camera as
    SdfPath _selectedCameraPath; // => activeCameraPath

    // Position for the mouse in the viewport expressed in normalized unit
    double _mouseX = 0.0;
    double _mouseY = 0.0;
    GfVec2d GetMousePosition() { return { _mouseX, _mouseY }; }

    std::map<UsdStageRefPtr, UsdImagingGLEngine *> _renderers;

    UsdStageRefPtr _stage;
    UsdStageRefPtr GetCurrentStage() { return _stage; }
    void SetCurrentStage(UsdStageRefPtr stage) { _stage = stage; }

    SelectionHash _lastSelectionHash = 0;

    /// Simple state machine for handling events
    void HandleEvents(Selection &);

    // These are the "states"
    void HandleHovering(Selection & );   // Hovering
    void HandleCameraEditing(Selection & ); // CameraEdition
    void HandleManipulatorEditing(Selection & );  // ManipulatorEdition
    void HandleSelecting(Selection & );// Picking

    HandleEventsFunction _handleEventFunction;
};

