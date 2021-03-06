
# usdtweak

A free and open source Pixar's USD editor, still in early development.

The UI is built with [ImGUI](https://github.com/ocornut/imgui) and is fully C++/OpenGL. The code is organised such that it should eventually be possible to reuse the widgets in other C++ projects using OpenGL and USD.

## Sneak peek

![screenshot1](https://media.giphy.com/media/9Nb4JmmqEXzO05DpvL/giphy.gif)

## Status

The editor allows

- editing multiple stages and layers
- adding, deleting, reparenting, renaming sdf prims in layers
- creating and deleting variants
- adding references and payloads, inherits, ...
- changing property values
- adding and deleting keys
- translating, rotating, scaling objects in the viewport.
- and more ...

They are still missing features and the editor hasn't faced users yet but I believe it might be useful in some situations, to replace text editors for small fixes, or to author a simple master stage. One of the original idea was to directly edit layers without the need of knowing the USD syntax, and the Layer editor has the basic functionalities for that. Drop me an email if you are interested in beta testing.

## Building

### Requirement

The project is almost self contained and only needs:

- cmake > 3.6 and a C++14 compiler installed
- a build of [Universal Scene Description](https://github.com/PixarAnimationStudios/USD/releases/tag/v21.02) version 21.02
- a build of [GLFW](https://www.glfw.org/), version 3.3.2 works

### Compiling

If you managed to build USD, compiling usdtweak should be straightforward, cmake needs only 2 variables:

- __pxr_DIR__ pointing to the USD installation directory containing the file pxrConfig.cmake
- __glfw3_DIR__  pointing to the USD installation directory containing the file glfw3Config.cmake

on linux it goes along the lines of:

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake -Dpxr_DIR=/installs/usd-21.02 -Dglfw3_DIR=/installs/glfw-3.3.2/lib/cmake/glfw3 ..
    make

It should compile successfully on Windows 10 with MSVC 19, CentOS 7 with g++ and MacOS Catalina. The viewport doesn't work on mac as the OpenGL version is not supported, but the layer editor does.

## Contact

If you want to know more, drop me an email: cpichard.github@gmail.com
