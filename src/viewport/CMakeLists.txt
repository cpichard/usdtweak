
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/FlyCameraManipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/FlyCameraManipulator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/OrbitCameraManipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/OrbitCameraManipulator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CameraRig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/CameraRig.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Grid.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Grid.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ImagingSettings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ImagingSettings.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Manipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Manipulator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ManipulatorToolbox.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ManipulatorToolbox.h
    ${CMAKE_CURRENT_SOURCE_DIR}/MouseHoverManipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/MouseHoverManipulator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Playblast.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Playblast.h
    ${CMAKE_CURRENT_SOURCE_DIR}/PositionManipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/PositionManipulator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/RotationManipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/RotationManipulator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ScaleManipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ScaleManipulator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SelectionManipulator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SelectionManipulator.h
    ${CMAKE_CURRENT_SOURCE_DIR}/SceneObjectDrawer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/SceneObjectDrawer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Viewport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Viewport.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ViewportSettings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ViewportSettings.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ViewportCameras.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ViewportCameras.h
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
