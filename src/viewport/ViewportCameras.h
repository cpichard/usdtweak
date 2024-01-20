#pragma once

#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/camera.h>

PXR_NAMESPACE_USING_DIRECTIVE

// Manage and store the cameras used in the viewport.
// They can be stage cameras or usdtweak owned cameras
//
// It should probably also list the new cameras
// the list of cameras available in the viewport (need a stage)
// Keep a track of which camera is selected for each stage
// Return the selected camera for rendering
// Set the new selected camera
// Having its internal cams:Persp/Top/Bottom as static so they can be shared
//
class ViewportCameras {
public:
    ViewportCameras() : _selectedCameraPath(perspectiveCameraPath), _renderCamera(&_perspectiveCamera) {}

    // Should we have an update ??
    void Update(const UsdStageRefPtr &stage, UsdTimeCode tc);
    
    void SetCameraAspectRatio(int width, int height);
    void DrawCameraList(const UsdStageRefPtr &stage);

    // Returning the editable camera which can be modified externaly by manipulators or other functions
    GfCamera & GetEditableCamera() { return *_renderCamera; }
    const GfCamera & GetCurrentCamera() const { return *_renderCamera; }
    
    void SetCameraPath(const UsdStageRefPtr &stage, const SdfPath &cameraPath);
    const SdfPath & GetSelectedCameraPath() const {return _selectedCameraPath;}

    //
    inline bool IsPerspective() const { return _selectedCameraPath == perspectiveCameraPath; }
    
    
private:
    SdfPath _selectedCameraPath;
    GfCamera *_renderCamera;    // Points to a valid camera, stage or perspective
    GfCamera _stageCamera;      // A copy of the current stage camera for editing

    // TODO: if we want to have multiple viewport, the persp camera shouldn't belong to the viewport but
    // another shared object, CameraList or similar
    // We could also have a "universe camera" common to all stage and viewports
    GfCamera _perspectiveCamera; // opengl this could be static ??


    //
    static SdfPath perspectiveCameraPath;
    // TODO replace SdfPath by enum {Perspective, Top, Bottom, Left, Right};
    
    // Stage selected camera
    // no stage -> selected camera (ut camera)
    // stage -> selected camera (stage camera or ut camera)

};

// Draw a list of viewport owned cameras: Perpective, Top, Bottom, etc. Not the stage cameras
// and allow selection of one of them
//void DrawViewportCameras(ViewportCamera &viewport);

// Draw a UI with the list of cameras and allows to select one of them as the viewport camera
// void DrawStageCameras(const UsdStage &stage, ViewportCamera &viewport);
