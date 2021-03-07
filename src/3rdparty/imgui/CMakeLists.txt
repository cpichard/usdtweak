
target_sources(usdtweak PRIVATE
 ${CMAKE_CURRENT_SOURCE_DIR}/imgui.cpp
 ${CMAKE_CURRENT_SOURCE_DIR}/imgui_draw.cpp
 ${CMAKE_CURRENT_SOURCE_DIR}/imgui_impl_glfw.cpp
 ${CMAKE_CURRENT_SOURCE_DIR}/imgui_impl_opengl3.cpp
 ${CMAKE_CURRENT_SOURCE_DIR}/imgui_stdlib.cpp
 ${CMAKE_CURRENT_SOURCE_DIR}/imgui_tables.cpp
 ${CMAKE_CURRENT_SOURCE_DIR}/imgui_widgets.cpp
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
