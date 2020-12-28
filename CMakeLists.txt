cmake_minimum_required (VERSION 3.6)
project (UsdTweak)
set (CMAKE_CXX_STANDARD 14)
find_package(glfw3 3.2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(pxr REQUIRED)
add_executable(usdtweak
 src/main.cpp
 src/Editor.cpp
 src/Selection.cpp
 src/commands/Commands.cpp
 src/commands/UndoLayerStateDelegate.cpp
 src/commands/SdfUndoRecorder.cpp
 src/commands/SdfCommandGroup.cpp
 src/commands/SdfLayerInstructions.cpp
 src/viewport/CameraRig.cpp
 src/viewport/CameraManipulator.cpp
 src/viewport/Grid.cpp
 src/viewport/PositionManipulator.cpp
 src/viewport/SelectionManipulator.cpp
 src/viewport/Viewport.cpp
 src/viewport/Manipulator.cpp
 src/viewport/MouseHoverManipulator.cpp
 src/widgets/ModalDialogs.cpp
 src/widgets/LayerEditor.cpp
 src/widgets/FileBrowser.cpp
 src/widgets/PropertyEditor.cpp
 src/widgets/StageOutliner.cpp
 src/widgets/Timeline.cpp
 src/widgets/Theater.cpp
 src/widgets/PrimSpecEditor.cpp
 src/widgets/ValueEditor.cpp
 src/widgets/RendererSettings.cpp
 src/3rdparty/imgui/imgui.cpp
 src/3rdparty/imgui/imgui_draw.cpp
 src/3rdparty/imgui/imgui_impl_glfw.cpp
 src/3rdparty/imgui/imgui_widgets.cpp
 src/3rdparty/imgui/imgui_impl_opengl3.cpp)
target_compile_definitions(usdtweak PRIVATE NOMINMAX)
target_link_libraries(usdtweak glfw ${OPENGL_gl_LIBRARY} ${PXR_LIBRARIES})
target_include_directories(usdtweak PUBLIC ${OPENGL_INCLUDE_DIR} ${PXR_INCLUDE_DIRS} 
 src 
 src/commands
 src/widgets 
 src/viewport 
 src/3rdparty/imgui 
 src/3rdparty/filesystem)
