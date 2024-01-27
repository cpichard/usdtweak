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
// TODO have camera per stage
//
class ViewportCameras {
public:
    ViewportCameras() : _selectedCameraPath(perspectiveCameraPath), _renderCamera(&_perspectiveCamera) {}

    // The update is called at every rendered frame, it applies the changes done to this structure
    void Update(const UsdStageRefPtr &stage, UsdTimeCode tc);
    
    void SetCameraAspectRatio(int width, int height);
    
    // UI
    void DrawCameraList(const UsdStageRefPtr &stage);
    void DrawCameraEditor(const UsdStageRefPtr &stage, UsdTimeCode tc);
    
    // Accessors
    std::string GetCurrentCameraName() {return _selectedCameraPath.GetName();}
    
    // Returning an editable camera which can be modified externaly by manipulators or any functions,
    // in the viewport. This will likely be reset at each frame. This is not thread safe and should be used only
    // in the main render loop.
    GfCamera & GetEditableCamera() { return *_renderCamera; }
    // GetSelectedGfCamera()
    const GfCamera & GetCurrentCamera() const { return *_renderCamera; }
       
    //
    const SdfPath & GetStageCameraPath() const ;


    // TODO CurrentIsPerspectiveCamera()
    // IsUsingPerpectiveCamera()
    // SelectedCameraIsPerspective()
    inline bool IsPerspective() const { return _selectedCameraPath == perspectiveCameraPath; }
    
    // TODO IsEditingStageCamera()
    inline bool IsUsingStageCamera () const {
        return _renderCamera != &_perspectiveCamera;
    }

private:
    
    // Stage camera path
    void SetStageAndCameraPath(const UsdStageRefPtr &stage, const SdfPath &cameraPath);
    
    SdfPath _selectedCameraPath;
    GfCamera *_renderCamera;    // Points to a valid camera, stage or perspective
    GfCamera _stageCamera;      // A copy of the current stage camera for editing

    // TODO: if we want to have multiple viewport, the persp camera shouldn't belong to the viewport but
    // another shared object, CameraList or similar
    // We could also have a "universe camera" common to all stage and viewports
    GfCamera _perspectiveCamera; // opengl this could be static, shared with all the viewports ??


    //
    static SdfPath perspectiveCameraPath;
    // TODO replace SdfPath by enum {Perspective, Top, Bottom, Left, Right};
    
    // Stage selected camera
    // no stage -> selected camera (ut camera)
    // stage -> selected camera (stage camera or ut camera)

};
