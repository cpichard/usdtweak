cmake_minimum_required (VERSION 3.14)
project(usdtweak)
set(CMAKE_CXX_STANDARD 14)
find_package(OpenGL REQUIRED)
find_package(pxr REQUIRED)

# for glfw, if the user doesn't specify the glfw install dir, we attempt to
# download the latest version and add it to the project directly
if (DEFINED glfw3_DIR)
    find_package(glfw3 3.2 REQUIRED)
else ()
    include(FetchContent)
    FetchContent_Declare(
        glfw
        URL https://github.com/glfw/glfw/releases/download/3.3.6/glfw-3.3.6.zip
    )
    set(GLFW_BUILD_EXAMPLES OFF)
    set(GLFW_BUILD_TESTS OFF)
    set(GLFW_BUILD_DOCS OFF)
    FetchContent_MakeAvailable(glfw)
endif (DEFINED glfw3_DIR)


# Use folders in the generated xcode/vs projects
set_property(GLOBAL PROPERTY USE_FOLDERS ON)


add_executable(usdtweak "")
add_subdirectory(src)


target_compile_definitions(usdtweak PRIVATE NOMINMAX)
target_link_libraries(usdtweak glfw resources ${OPENGL_gl_LIBRARY} ${PXR_LIBRARIES})
target_include_directories(usdtweak PUBLIC ${OPENGL_INCLUDE_DIR} ${PXR_INCLUDE_DIRS})

# Installer on windows
if(WIN32)
    include("cmake/windows_installer.cmake")
endif()

# Remove warnings coming from usd and enable default multithreaded compilation on windows
target_compile_options(usdtweak PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/MP /wd4244 /wd4305 /wd4996>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-deprecated>)

# Organise the sources using the same hierarchy as the filesystem in xcode and vs projects
get_target_property(USDTWEAK_SOURCES usdtweak SOURCES)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PREFIX "src"
    FILES ${USDTWEAK_SOURCES})
