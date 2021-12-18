cmake_minimum_required (VERSION 3.11)
project(UsdTweak)
set(CMAKE_CXX_STANDARD 14)
find_package(glfw3 3.2 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(pxr REQUIRED)

# Use folders in the generated xcode/vs projects
set_property(GLOBAL PROPERTY USE_FOLDERS ON)


add_executable(usdtweak "")
add_subdirectory(src)


target_compile_definitions(usdtweak PRIVATE NOMINMAX)
target_link_libraries(usdtweak glfw resources ${OPENGL_gl_LIBRARY} ${PXR_LIBRARIES})
target_include_directories(usdtweak PUBLIC ${OPENGL_INCLUDE_DIR} ${PXR_INCLUDE_DIRS})

# Remove warnings coming from usd and enable default multithreaded compilation on windows
target_compile_options(usdtweak PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/MP /wd4244 /wd4305 /wd4996>
	$<$<CXX_COMPILER_ID:GNU>:-Wno-deprecated>)

# Organise the sources using the same hierarchy as the filesystem in xcode and vs projects
get_target_property(USDTWEAK_SOURCES usdtweak SOURCES)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PREFIX "src"
    FILES ${USDTWEAK_SOURCES})
