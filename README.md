
# usdtweak

usdtweak is a free and open source editor for Pixar's [USD](https://graphics.pixar.com/usd/release/index.html#) format. The project is still in early stage, but usdtweak can already be used for small and simple tasks like cleaning assets, creating and editing layers or inspecting and fixing existing usd stages.

This project is written in C++ and relies on [ImGUI](https://github.com/ocornut/imgui) for the UI and [GLFW](https://github.com/glfw/glfw) for the windowing system.

## Sneak peek

![screenshot1](https://media.giphy.com/media/9Nb4JmmqEXzO05DpvL/giphy.gif)

## Status

There is no roadmap at the moment, as it is my side project and the development is slow and unpredictable, I can only work on it a few hours during the week-end. However the original idea was to improve usdview by adding edition capabilities, for artists, technical directors and users who don't necessarily know the USD ascii syntax or are not familiar with python. So the current goal driving the developments is providing the same functionalities as usdview but with edition capabilities.

As of today usdtweak allows

- to edit multiple stages and layers at the same time, copying and pasting prims between layers,
- to edit stages properties selecting the edit target
- to edit layer hierarchy: adding, deleting, reparenting, and renaming prims
- to edit layer stack: adding, deleting new sublayers
- to create and delete compositions like variants, references and payloads, inherits, ...
- to change property values in layers or stages
- to add and delete keys
- a minimal viewport interaction: translating, rotating, scaling objects.
- text editing (for small files)
- and more ...

Drop me an email if you are interested in beta testing on windows or if you can't compile it.

## Building

### Requirement

The project is almost self contained and only needs:

- [cmake](https://cmake.org/) installed (version > 3.14)
- a C++14 compiler installed: MSVC 19 or 17, g++ or clang++.
- a build of [Universal Scene Description](https://github.com/PixarAnimationStudios/USD/releases/tag/v22.05) version >= 20.11

If you managed to build USD, compiling usdtweak should be easy, cmake needs only 1 required variables:

- __pxr_DIR__ pointing to the USD installation directory containing the file pxrConfig.cmake

Note that usdtweak hasn't been compiled with the usd libraries provided with maya, houdini or omniverse, even if it might be possible to do so.

### Compiling on linux

On linux it should compile with:

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake -Dpxr_DIR=/installs/usd-22.05 ..
    make

### Compiling on MacOs

It compiles on MacOs Catalina. The viewport doesn't work as the required OpenGL version is not supported, but the layer editor does.

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake -Dpxr_DIR=/installs/usd-22.05 ..
    make

### Compiling on Windows

It should compile successfully on Windows 10 with MSVC 19 or 17 using the RelWithDbInfo config. Make sure you open/use the x64 Native Tools commands prompt before typing the following commands:

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\installs\usd-22.05 ..
    cmake --build . --config RelWithDebInfo

### Installing on Windows

You can install usdtweak with its dependencies on windows, it copies the required files in a directory with the following command:

    cmake --install . --prefix <where_you_want_to_install_usdtweak> --config RelWithDebInfo

 Note that it is not really tested on anything else than my machine/setup so it might not work for you, feel free to get in touch if you have any issue.

### Compiling with your version of glfw

usdtweak is using [GLFW](https://www.glfw.org/) for its windowing system. cmake should normally download, compile and install glfw without any user intervention. However, if you already have a compiled version you want to use instead, and you'll need to disable the automatic build of glfw, by passing an additional cmake variable:

- __glfw3_DIR__  pointing to your GLFW installation directory and containing the file glfw3Config.cmake

A cmake command will then look like:

    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\installs\usd-21.11 -Dglfw3_DIR=C:\installs\glfw3-3.3.6\lib\cmake\glfw3 ..

## Contact

If you want to know more, or have any issues, questions, drop me an email: cpichard.github@gmail.com
