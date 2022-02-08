
# usdtweak

usdtweak is a free and open source Pixar's USD editor, still in early development, but it can already be used for small and simple tasks like cleaning asset coming from blender, creating new layers or even inspecting existing usd files.

This project is fully C++/OpenGL and relies on [ImGUI](https://github.com/ocornut/imgui) for the UI.

## Sneak peek

![screenshot1](https://media.giphy.com/media/9Nb4JmmqEXzO05DpvL/giphy.gif)

## Status

The editor allows

- to edit multiple stages and layers at the same time, copying and pasting prims between layers.
- to edit the layer hierarchy: adding, deleting, reparenting, and renaming prims
- to create and delete variants, references and payloads, inherits, ...
- to change property values in layers or stage. The arrays are not editable yet
- to adding and deleting keys
- a minimal viewport interaction: translating, rotating, scaling objects.
- text editing for small files
- and more ...

The original idea was to be able to edit layers without knowing the USD syntax, and the layer editors have some of the basic functionalities for that. As this is my side project and I can only work on it a few hours during the week-end, the development is slow, but steady. Drop me an email if you are interested in beta testing or if you can't compile it.

## Building

### Requirement

The project is almost self contained and only needs:

- cmake installed (version > 3.14)
- a C++14 compiler installed: MSVC 19 or 17, gcc or llvm.
- a build of [Universal Scene Description](https://github.com/PixarAnimationStudios/USD/releases/tag/v21.11) version 21.11

If you managed to build USD, compiling usdtweak should be straightforward, cmake needs only 1 required variables:

- __pxr_DIR__ pointing to the USD installation directory containing the file pxrConfig.cmake

### Compiling on linux

On linux it should compile with:

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake -Dpxr_DIR=/installs/usd-21.11 ..
    make

### Compiling on MacOs

It compiles on MacOs Catalina. The viewport doesn't work as the required OpenGL version is not supported, but the layer editor does.

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake -Dpxr_DIR=/installs/usd-21.11 ..
    make

### Compiling on Windows

It should compile successfully on Windows 10 with MSVC 19 or 17. Make sure you open/use the x64 Native Tools commands prompt before typing the following commands:

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\installs\usd-21.11 ..
    cmake --build . --config RelWithDebInfo

### Compiling with your version of glfw

Under the hood usdtweak is using [GLFW](https://www.glfw.org/). Cmake should normally download, compile and install glfw without any intervention. However, if you already have a compiled version you want to use, and you'll need to disable the automatic build of glfw, by passing an additional cmake variable:

- __glfw3_DIR__  pointing to the USD installation directory containing the file glfw3Config.cmake

A cmake command will then look like:

    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\installs\usd-21.11 -Dglfw3_DIR=C:\installs\glfw3-3.3.6\lib\cmake\glfw3 ..

## Contact

If you want to know more, drop me an email: cpichard.github@gmail.com
