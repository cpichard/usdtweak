
# usdtweak

usdtweak is a free and open source editor for Pixar's [USD](https://graphics.pixar.com/usd/release/index.html#) format. The project is in alpha stage, but usdtweak can already be used for small and simple tasks like cleaning assets, creating and editing layers, inspecting and fixing existing usd stages.

This project is written in C++ and is made possible by [ImGUI](https://github.com/ocornut/imgui) for the UI and [GLFW](https://github.com/glfw/glfw) for the windowing system.

## Sneak peek

https://github.com/cpichard/usdtweak/assets/300243/3f34cd6f-de84-428f-9569-a1ac3bd61206

## Status

There is no roadmap, it is a side project and the development is slow and unpredictable, I can only work on it a few hours during the week-end, but anyone wanting to contribute is welcome. The original idea behind this project was to improve usdview by adding edition capabilities, for artists, technical directors and users who don't necessarily know the USD ascii syntax or are not familiar with python. So my current goal driving the developments is to provide at least the same functionalities as usdview with some ability to edit the stages and layers.

As of today usdtweak allows

- to edit multiple stages and layers at the same time, copying and pasting prims between layers,
- to edit stages properties selecting the edit target
- to create and edit variants
- to edit layer hierarchy: adding, deleting, reparenting, and renaming prims
- to edit layer stack: adding, deleting new sublayers
- to create and delete compositions like references and payloads, inherits, ...
- to change property values in layers or stages
- to add and delete keys on properties
- a minimal viewport interaction: translating, rotating, scaling objects.
- text editing (for small files)
- and more ...

If you want to try usdtweak without the burden of compiling it, you can download the latest windows installer here https://github.com/cpichard/usdtweak/releases. Feel free to reach out if you have feedbacks.

You can also build usdtweak, here are the instructions to do so. 

## Building

### Requirement

The project is almost self contained and only needs:

- [cmake](https://cmake.org/) installed (version > 3.14)
- a C++17 compiler installed: MSVC 19 or 17, g++ or clang++.
- a build of [Universal Scene Description](https://github.com/PixarAnimationStudios/USD/releases/tag/v23.11) version >= 20.11. In theory the USD libraries provided with maya or houdini should work but they are not tested, let me know if you manage to compile with them.

If you built USD, compiling usdtweak should be easy, you need to provide cmake with only 1 required variables:

- __pxr_DIR__ pointing to the USD installation directory containing the file pxrConfig.cmake

Unfortunately if you have compiled USD >= 22.08 with MaterialX you will also need to provide:

- __MaterialX_DIR__ pointing to the MaterialX installation directory containing the file MaterialXConfig.cmake.


### Compiling on linux

On linux it should compile with:

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake -Dpxr_DIR=/path/to/usd-23.11 ..
    make

If you have USD >= 22.08 compiled with MaterialX, cmake becomes:

    cmake -Dpxr_DIR=/path/to/usd-23.11 -DMaterialX_DIR=/path/to/usd-23.11/lib/cmake/MaterialX ..


### Compiling on MacOs

It compiles on MacOS Mojave. The viewport is now enabled for versions of USD superior or equal to 22.08, otherwise it is deactivated because the OpenGL version is not supported on MacOS;

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake -Dpxr_DIR=/path/to/usd-23.11 ..
    make

If you have USD >= 22.08 compiled with MaterialX, cmake becomes:

    cmake -Dpxr_DIR=/path/to/usd-23.11 -DMaterialX_DIR=/path/to/usd-23.11/lib/cmake/MaterialX ..

### Compiling on Windows

It should compile successfully on Windows 10 with MSVC 19 or 17 using the RelWithDbInfo config. Make sure you open/use the x64 Native Tools commands prompt before typing the following commands:

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\path\to\usd-23.11 ..
    cmake --build . --config RelWithDebInfo

If you have USD >= 22.08 compiled with MaterialX, you have to add an additional MaterialX_DIR variable to the cmake command, pointing to the MaterialX directory:

    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\path\to\usd-22.08 -DMaterialX_DIR=C:\path\to\usd-22.08\lib\cmake\MaterialX ..

#### Using the NVidia USD build (experimental)

NVidia provides a USD build [here](https://developer.nvidia.com/usd) if you don't want to compile USD yourself. It needs either VisualStudio 2017 or a more recent version (2019, 2022) with the "MSVC141 - C++ build tools x86/x64" installed. The cmake commands to build usdtweak differ, if you have a more recent version you'll need to specify the toolkit using `-T v141`. The Nvidia USD build also needs Python3.7, set the `USE_PYTHON3` argument to force cmake to look after Python3, but you'll have to make sure Python3.7 is installed already.

    cmake  -G "Visual Studio 16 2019" -T v141 -A x64 -Dpxr_DIR=C:\path\to\nvidia-usd-22.11 -DMaterialX_DIR=C:\path\to\nvidia-usd-22.11\lib\cmake\MaterialX -DUSE_PYTHON3=ON ..
    cmake --build . --config Release

### Installing on Windows

You can install usdtweak with its dependencies on windows, it copies the required files in a directory with the following command:

    cmake --install . --prefix <where_you_want_to_install_usdtweak> --config RelWithDebInfo

 Note that it is not really tested on anything else than my machine/setup so it might not work for you, feel free to get in touch if you have any issue.

### Creating a Windows installer

There is an experimental packaging system using cpack/NSIS on windows which creates an installer. You have to make sure the nsis application is available on you system, you can download it from here [NSIS](https://nsis.sourceforge.io/Download). The command to create the installer is then:

    cmake --build . --target package --config RelWithDebInfo

### Compiling with your version of glfw

usdtweak is using [GLFW](https://www.glfw.org/) for its windowing system. cmake should normally download, compile and install glfw without any user intervention. However, if you already have a compiled version you want to use instead, and you'll need to disable the automatic build of glfw, by passing an additional cmake variable:

- __glfw3_DIR__  pointing to your GLFW installation directory and containing the file glfw3Config.cmake

A cmake command will then look like:

    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\path\to\usd-21.11 -Dglfw3_DIR=C:\path\to\glfw3-3.3.6\lib\cmake\glfw3 ..

## Known issues

- When enabling the scene materials in Storm, the texture don't always load correctly. This can be solved by setting the USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX environment variable to 1.

## Contact

If you want to know more, or have any issues, questions, drop me an email: cpichard.github@gmail.com
