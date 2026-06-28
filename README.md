
# usdtweak

usdtweak is a free and open source editor for [OpenUSD](https://graphics.pixar.com/usd/release/index.html#). usdtweak can already be used for small and simple tasks like cleaning or preparing assets, creating and editing layers, inspecting and fixing usd stages. It works on windows, macos and linux.

This project is written in C++ and is powered by [ImGUI](https://github.com/ocornut/imgui) for the UI and [GLFW](https://github.com/glfw/glfw) for the windowing system.

## Sneak peek

https://github.com/cpichard/usdtweak/assets/300243/3f34cd6f-de84-428f-9569-a1ac3bd61206

## Status

usdtweak is a side project and the development follows its own pace. The original idea was to improve usdview by adding edition capabilities, for artists, technical directors and users who don't know the OpenUSD ascii syntax and are not familiar with python. The current goal driving the developments is to provide at least the same functionalities as usdview with the ability to edit stages and layers.

As of today usdtweak allows
 
**Stage & layer search and editing**: search, browse and edit multiple stages and layers in the same application, copy and paste specs between layers, edit layer hierarchy (adding, deleting, reparenting, and renaming specs), and manage the stage layer stack (adding and deleting sublayers).
 
**Composition & variants**: create and delete compositions like references, payloads, and inherits, and create and edit variants at the layer level.
 
**Properties & animation**: edit stage properties in an edit target context, change property values in layers and stages, and add and delete keys on properties.
 
**Materials & viewport**: assign materials on a prim and interact with the viewport (translating, rotating, scaling objects).
 
**Text editing**: edit any layer with a text editor, navigate layers like a web browser.

**Experimental features**: visualize and edit connection graphs, useful for shader/materials graphs. A natural language agent name twiki, that can help you find issues in the scene or do bulk modifications.

If you want to try usdtweak without the burden of compiling it, you can download the latest installer here https://github.com/cpichard/usdtweak/releases. Feel free to [reach out](#contact) if you have any issue with it (or success).

## Building
If you're on windows, you can use `windows-build.ps1` powershell script to build usdtweak. It will download the OpenUSD pre-build binaries from [NVIDIA](https://developer.nvidia.com/usd#bin) and compile usdtweak for you, granted you have the required tools installed (Cmake, Visual Studio 2022 build tools).
Keep in mind nvidiaOpenUSDbinaries are under [NVIDIA OMNIVERSE LICENSING](https://www.nvidia.com/en-us/agreements/enterprise-software/nvidia-software-license-agreement/), make sure you're aware of the terms and conditions before using them.

After running the build script, you can run `windows-start.bat` to start usdtweak.

If you prefer to compile usdtweak manually, you can follow the instructions in the [Building.md](doc/Building.md) file:

 - [Building requirements](doc/Building.md#requirements)
 - [Building on windows](doc/Building.md#compiling-on-windows)
 - [Building on macos](doc/Building.md#compiling-on-macos)
 - [Building on linux](doc/Building.md#compiling-on-linux)

## Documentation

The documentation now lives in the growing [wiki](https://github.com/cpichard/usdtweak/wiki), it mainly contains informations on how to use usdtweak. 

## Extending usdtweak

With the recent rise of the coding assistants, researchers, technical artists, and artists can build dedicated tools and interfaces without knowledge of coding language. usdtweak now allows you to easily write C++ [addons](doc/Addons.md) for your specific needs, using the main application as a foundation. addons are similar to plugins, but maintained by you and without the burden of managing dynamic libraries, you build the application with your tools.

## Contributing

This project welcomes any contributions: features ideas, bug fixes, documentation, tutorials, etc. If you want to contribute just [reach out by mail](#contact).

For code contribution you can make a pull request from your fork. If you don't know how to fork and create pull requests, this [video](https://www.youtube.com/watch?v=nT8KGYVurIU) explains the process.

## Known issues

- When enabling the scene materials in Storm, the texture don't always load correctly. This can be solved by setting the USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX environment variable to 1.

## Contact

If you want to know more, or have any issues, questions, drop me an email: cpichard.github@gmail.com or fill a [github issue](https://github.com/cpichard/usdtweak/issues/new).

## Thanks

Big thanks all the humans who contributed to this project, making it better months after months. Special thanks to @oumad, @AlexRamallo for the discussions, design ideas, docs, videos, and much more and @ChubbyQuark for the big push on the wiki which has become really helpful.