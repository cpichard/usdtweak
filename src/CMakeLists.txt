
target_sources(usdtweak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Constants.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandLineOptions.h
    ${CMAKE_CURRENT_SOURCE_DIR}/CommandLineOptions.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Debug.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Debug.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Editor.h
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorSettings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/EditorSettings.h
    ${CMAKE_CURRENT_SOURCE_DIR}/GeometricFunctions.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Gui.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ImGuiHelpers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ImGuiHelpers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/ProxyHelpers.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Selection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/Selection.h
    ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
)

target_include_directories(usdtweak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(commands)
add_subdirectory(3rdparty)
add_subdirectory(resources)
add_subdirectory(widgets)
add_subdirectory(viewport)
