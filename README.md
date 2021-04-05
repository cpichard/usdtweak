
# usdtweak

A free and open source Pixar's USD editor, still in early development.

The UI is built with [ImGUI](https://github.com/ocornut/imgui) and is fully C++/OpenGL. The code is organised such that it should eventually be possible to reuse the widgets in other OpenGL+USD projects.

![screenshot1](https://media.giphy.com/media/9Nb4JmmqEXzO05DpvL/giphy.gif)

## Status

The editor allows to edit multiple stages and layers, add, delete, reparent, rename prims in layers, add references and payloads, change values, add keys, move objects in the viewport, etc. They are still a lot of missing features and the editor hasn't been tested in production but it can be useful for quick change. One of the original idea was to directly edit layers like you would with a text editor, but without the need of knowing the syntax and with a better browsing experience. The development is slow as this one of my side project outside of work and I dont't spend much time on it at the moment.

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

It should compile successfully on Windows 10 with MSVC 19, Centos 7 with g++ and MacOS Catalina. The viewport doesn't work on mac as the OpenGL version is not supported, but the layer editor does.

## Contact

If you want to know more, drop me an email: cpichard.github@gmail.com
