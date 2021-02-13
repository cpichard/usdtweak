cmake_minimum_required (VERSION 3.6)
project (UsdTweak)
set (CMAKE_CXX_STANDARD 14)
find_package(glfw3 3.2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(pxr REQUIRED)
add_executable(usdtweak
 src/3rdparty/imgui/imgui.cpp
 src/3rdparty/imgui/imgui_draw.cpp
 src/3rdparty/imgui/imgui_impl_glfw.cpp
 src/3rdparty/imgui/imgui_impl_opengl3.cpp
 src/3rdparty/imgui/imgui_stdlib.cpp
 src/3rdparty/imgui/imgui_tables.cpp
 src/3rdparty/imgui/imgui_widgets.cpp
 src/Editor.cpp
 src/Editor.h
 src/Gui.h
 src/ImGuiHelpers.h
 src/ProxyHelpers.h
 src/Selection.cpp
 src/Selection.h
 src/commands/Commands.cpp
 src/commands/Commands.h
 src/commands/SdfCommandGroup.cpp
 src/commands/SdfCommandGroup.h
 src/commands/SdfLayerInstructions.cpp
 src/commands/SdfLayerInstructions.h
 src/commands/SdfUndoRecorder.cpp
 src/commands/SdfUndoRecorder.h
 src/commands/UndoLayerStateDelegate.cpp
 src/commands/UndoLayerStateDelegate.h
 src/main.cpp
 src/resources/ResourcesLoader.h
 src/resources/ResourcesLoader.cpp
 src/viewport/CameraManipulator.cpp
 src/viewport/CameraManipulator.h
 src/viewport/CameraRig.cpp
 src/viewport/CameraRig.h
 src/viewport/Grid.cpp
 src/viewport/Grid.h
 src/viewport/Manipulator.cpp
 src/viewport/Manipulator.h
 src/viewport/MouseHoverManipulator.cpp
 src/viewport/MouseHoverManipulator.h
 src/viewport/PositionManipulator.cpp
 src/viewport/PositionManipulator.h
 src/viewport/RotationManipulator.cpp
 src/viewport/RotationManipulator.h
 src/viewport/ScaleManipulator.cpp
 src/viewport/ScaleManipulator.h
 src/viewport/SelectionManipulator.cpp
 src/viewport/SelectionManipulator.h
 src/viewport/Viewport.cpp
 src/viewport/Viewport.h
 src/widgets/CompositionEditor.cpp
 src/widgets/CompositionEditor.h
 src/widgets/FileBrowser.cpp
 src/widgets/FileBrowser.h
 src/widgets/LayerEditor.cpp
 src/widgets/LayerEditor.h
 src/widgets/ModalDialogs.cpp
 src/widgets/ModalDialogs.h
 src/widgets/PrimSpecEditor.cpp
 src/widgets/PrimSpecEditor.h
 src/widgets/PropertyEditor.cpp
 src/widgets/PropertyEditor.h
 src/widgets/RendererSettings.cpp
 src/widgets/RendererSettings.h
 src/widgets/StageOutliner.cpp
 src/widgets/StageOutliner.h
 src/widgets/Theater.cpp
 src/widgets/Theater.h
 src/widgets/Timeline.cpp
 src/widgets/Timeline.h
 src/widgets/ValueEditor.cpp
 src/widgets/ValueEditor.h
)
target_compile_definitions(usdtweak PRIVATE NOMINMAX)
target_link_libraries(usdtweak glfw ${OPENGL_gl_LIBRARY} ${PXR_LIBRARIES})
target_include_directories(usdtweak PUBLIC ${OPENGL_INCLUDE_DIR} ${PXR_INCLUDE_DIRS} 
 src 
 src/commands
 src/widgets 
 src/viewport 
 src/resources
 src/3rdparty/imgui 
 src/3rdparty/filesystem
 src/3rdparty/iconfontcppheaders
 src/3rdparty/fontawesome
)
# Remove warnings coming from usd and enable default multithreaded compilation on windows
target_compile_options(usdtweak PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/MP /wd4244 /wd4305>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-deprecated>)
