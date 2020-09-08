usdtweak
=========
A free and open source experimental usd editor eventually coming with a set of re-usable widgets. Still in early development.

The UI is based on [ImGUI](https://github.com/ocornut/imgui) and is fully C++/OpenGL. It should be possible to integrate it easyly into any DCC or in-house projects using OpenGL. 


Status
------
It started as a week-end hack to mess around with USD and ImGUI, and grew to the point it can be useful and shared. The editor is a prototype and should be seen as a sandbox to experiment USD UI workflows rather than a proper editing system yet. It is possible to edit multiple stages and layers at the same time, and do simple editions like adding or deleting prims in layers, add references, translates, etc. but most of the features are missing and the architectural work is still ongoing. Many widgets are in their enfancy, but feel free to clone the code and hack it !

Requirement
-----------
The project is almost self contained and only needs:
 - cmake installed, with a C++14 compiler
 - A build of the latest [USD](https://github.com/PixarAnimationStudios/USD/releases/tag/v20.08) version
 - An installed version of [GLFW](https://www.glfw.org/)

Building the code
-----------------
If you managed to build USD, building usdtweak should be straightforward.

    git clone https://github.com/cpichard/usdtweak
    cd usdtweak
    git checkout develop
    mkdir build
    cd build
    cmake ..
    make

It compiles successfully on Windows10, Centos, MacOS Catalina. The Hydra viewport doesn't work on Catalina.

Contact
-------
If you are interest by this project, want to know more or contribute, drop me an email: cpichard.github@gmail.com
