# Building usdtweak

## Requirements

The project is almost self contained and only needs:

- [cmake](https://cmake.org/) installed (version > 3.14)
- a C++17 compiler installed: MSVC 19 or 17, g++ or clang++.
- a build of [Universal Scene Description](https://github.com/PixarAnimationStudios/USD/releases/tag/v26.05) version >= 20.11. In theory the OpenUSD libraries provided with mayausd should work but they are not tested, let me know if you manage to compile with them.

To compile usdtweak you normally need to provide cmake with only 1 required variables:

- __pxr_DIR__ pointing to the OpenUSD installation directory containing the file pxrConfig.cmake

Unfortunately if you have compiled a vanilla OpenUSD >= 22.08, depending on the OpenUSD version you might also need to provide:

- __MaterialX_DIR__ pointing to the MaterialX installation directory containing the file MaterialXConfig.cmake.
- __OpenSubdiv_DIR__ pointing to the OpenSubdiv installation directory containing the file OpenSubdivConfig.cmake.

OpenUSD config doesn't always add them as dependencies.

## Compiling on Linux or macOS

On linux the latest version should compile with the vanilla OpenUSD 26.05:

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake -Dpxr_DIR=/path/to/usd-26.05 -DOpenSubdiv_DIR=/path/to/usd-26.05/lib/cmake/OpenSubdiv ..
    make

## Compiling on Windows

It should compile successfully on Windows 10 with MSVC 19 or 17 using the RelWithDebInfo config. Make sure you open/use the x64 Native Tools commands prompt before typing the following commands:

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\path\to\usd-26.05 ..
    cmake --build . --config RelWithDebInfo

If you have OpenUSD >= 22.08 compiled with MaterialX, you have to add an additional MaterialX_DIR variable to the cmake command, pointing to the MaterialX directory:

    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\path\to\usd-22.08 -DOpenSubdiv_DIR=/path/to/usd-26.05/lib/cmake/OpenSubdiv ..

### Using NVidia's OpenUSD build (experimental)

NVidia provides a OpenUSD build [here](https://developer.nvidia.com/usd) if you don't want to compile OpenUSD yourself. We tested 2 versions, 22.11 and 24.08.

#### OpenUSD 24.08

This was tested on windows with MSVC2019. Unfortunatelly the configuration coming with the nvidia libraries doesn't have the correct python directories, so the cmake command is a bit more involved than with 22.11. Make sure you type the following commands in a x64 Native Tools commands prompt and replace `C:\path\to\nvidia-usd-24.08` by the actual path containing the nvidia usd libraries.

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\path\to\nvidia-usd-24.08 -DPython3_EXECUTABLE=C:\path\to\nvidia-usd-24.08\python\python.exe -DPython3_LIBRARY=C:\path\to\nvidia-usd-24.08\python\libs\python310.lib -DPython3_INCLUDE_DIR=C:\path\to\nvidia-usd-24.08\python\include -DPython3_VERSION="3.10.14" -DMaterialX_DIR=C:\path\to\nvidia-usd-24.08\lib\cmake\MaterialX -DImath_DIR=C:\path\to\nvidia-usd-24.08\lib\cmake\Imath ..

Then compile with:

    cmake --build . --config RelWithDebInfo

The build will fail with the following error:

    LINK : fatal error LNK1104: cannot open file 'C:\path\to\nvidia-usd-24.08\lib\osdGPU.lib' [C:\path\to\usdtweak\build-24.08-nvidia\usdtweak.vcxproj]

This is because the libraries `osdGPU.lib` and `osdCPU.lib` are not included in this nvidia release, but they are listed as dependencies in the configuration. To fix this problem you'll have to edit the file `C:\path\to\nvidia-usd-24.08\cmake\pxrTargets.cmake` and remove all occurence of `${_IMPORT_PREFIX}/lib/osdGPU.lib;` and all occurence of `${_IMPORT_PREFIX}/lib/osdCPU.lib;`. Once it's done you can recompile with

    cmake --build . --config RelWithDebInfo

And you should get usdtweak.exe compiled in the RelWithDebInfo folder. 

#### OpenUSD 22.11

It needs either VisualStudio 2017 or a more recent version (2019, 2022) with the "MSVC141 - C++ build tools x86/x64" installed. The cmake commands to build usdtweak differ, if you have a more recent version you'll need to specify the toolkit using `-T v141`. The Nvidia OpenUSD build also needs Python3.7, set the `USE_PYTHON3` argument to force cmake to look after Python3, but you'll have to make sure Python3.7 is installed already.

    cmake  -G "Visual Studio 16 2019" -T v141 -A x64 -Dpxr_DIR=C:\path\to\nvidia-usd-22.11 -DMaterialX_DIR=C:\path\to\nvidia-usd-22.11\lib\cmake\MaterialX -DUSE_PYTHON3=ON ..
    cmake --build . --config Release

### Using Houdini's OpenUSD build (experimental, only Houdini 20+ on Windows)

First make sure there is no OpenUSD and Python path in the environment variables. Open a [houdini command line shell](https://www.sidefx.com/faq/question/how-do-i-set-up-the-houdini-environment-for-command-line-tools/), inside the shell, create a build directory, like in the previous example then run the cmake command pointing pxr_DIR to the cmake\houdini subdirectory. 

    cmake -Dpxr_DIR=<USDTWEAK_DIR>\cmake\houdini ..
    cmake --build . --config RelWithDebInfo

(Replace <USDTWEAK_DIR> with the directory pointing to the usdtweak project)

## Installing on Windows

You can install usdtweak with its dependencies on windows, it copies the required files in a directory with the following command:

    cmake --install . --prefix <where_you_want_to_install_usdtweak> --config RelWithDebInfo

 Note that it is not really tested on anything else than my machine/setup so it might not work for you, feel free to get in touch if you have any issue.

## Creating a Windows installer

There is an experimental packaging system using cpack/NSIS on windows to create an installer. You have to make sure the nsis application is available on your system, you can download it from here [NSIS](https://nsis.sourceforge.io/Download). The command to create the installer is then:

    cmake --build . --target package --config RelWithDebInfo

## Compiling with your version of glfw

usdtweak is using [GLFW](https://www.glfw.org/) for its windowing system. cmake should normally download, compile and install glfw without any user intervention. However, if you already have a compiled version you want to use instead, and you'll need to disable the automatic build of glfw, by passing an additional cmake variable:

- __glfw3_DIR__  pointing to your GLFW installation directory and containing the file glfw3Config.cmake

A cmake command will then look like:

    cmake  -G "Visual Studio 16 2019" -A x64 -Dpxr_DIR=C:\path\to\usd-24.08 -Dglfw3_DIR=C:\path\to\glfw3-3.4\lib\cmake\glfw3 ..

