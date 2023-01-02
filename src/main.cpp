
// clang-format off
#include <iostream>
#include <cstdlib>
#ifdef WANTS_PYTHON
#include <Python.h>
#endif
#include <pxr/base/plug/registry.h>
#include <pxr/base/arch/env.h>
#include <pxr/base/arch/systemInfo.h>
#include <pxr/imaging/glf/contextCaps.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/diagnostic.h>
#include "Editor.h"
#include "Viewport.h"
#include "Commands.h"
#include "Constants.h"
#include "ResourcesLoader.h"
#include "CommandLineOptions.h"
#include "Gui.h"
// clang-format on

PXR_NAMESPACE_USING_DIRECTIVE

// https://learn.microsoft.com/en-us/windows/win32/procthread/changing-environment-variables
#ifdef _WIN64
static std::vector<char *> ArchCurrentEnviron() {
    std::vector<char *> newEnv;
    for (char *envIt = GetEnvironmentStrings(); *envIt; envIt++) {
        newEnv.push_back(envIt);
        while (*envIt != 0) {
            envIt++;
        }
    }
    newEnv.push_back(0); // The last element must be the null pointer
    return newEnv;
}
#endif

static bool InstallApplicationPluginPaths(const std::vector<std::string> &pluginPaths) {
    const char *BOOTSTRAPPED = "USDTWEAK_BOOTSTRAPPED";
    if (!pluginPaths.empty() && !ArchHasEnv(BOOTSTRAPPED)) {
        if (!ArchSetEnv(BOOTSTRAPPED, "1", true)) {
            return false;
        }
        ArchSetEnv("PXR_PLUGINPATH_NAME", TfStringJoin(pluginPaths.begin(), pluginPaths.end(), ";"), true);
        return true;
    }
    return false;
}

int main(int argc, char *const *argv) {

    CommandLineOptions options(argc, argv);

    // ResourceLoader will load the settings/fonts/textures and create an imgui context
    ResourcesLoader loader;

    // Adding the plugin paths specified in the config file to the environment. It potentially means restarting the
    // application with a new environment. Unfortunately USD is not able to dynamically load plugin 
    // functionalities after startup time, the functions like RegisterPlugins or Load simply does not
    // do what one would expect, more there:
    // https://groups.google.com/g/usd-interest/c/fpLYyf6elmU/m/haZf9bZDAgAJ
    // So the only option I see is to reload the application with an updated environment
    if (InstallApplicationPluginPaths(loader.GetEditorSettings()._pluginPaths)) {
        std::cout << "Reloading application with new environment" << std::endl;
        std::string exePath = ArchGetExecutablePath();
#ifndef _WIN64
        execve(exePath.c_str(), argv, ArchEnviron());
#else
        // Unfortunately ArchEnviron() doesn't return the modified environment on windows, we need to copy the current env
        _execve(exePath.c_str(), argv, ArchCurrentEnviron().data());
#endif
    }

    // Initialize python
#ifdef WANTS_PYTHON
    Py_SetProgramName(argv[0]);
    Py_Initialize();
#endif

    // Initialize glfw
    if (!glfwInit())
        return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#else
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
#endif

    /* Create a windowed mode window and its OpenGL context */
    int width = loader.GetApplicationWidth();
    int height = loader.GetApplicationHeight();

#ifdef DISABLE_DOUBLE_BUFFER
    glfwWindowHint(GLFW_DOUBLEBUFFER, GL_FALSE);
#endif
    GLFWwindow *window = glfwCreateWindow(width, height, "usdtweak", NULL, NULL);
    if (!window) {
        std::cerr << "unable to create a window, exiting" << std::endl;
        glfwTerminate();
        return -1;
    }

    // Make the window's context current
    glfwMakeContextCurrent(window);

    // Init glew with USD
    GarchGLApiLoad();
    GlfContextCaps::InitInstance();
    std::cout << glGetString(GL_VENDOR) << std::endl;
    std::cout << glGetString(GL_RENDERER) << std::endl;
    std::cout << "OpenGL " << glGetString(GL_VERSION) << std::endl;
    std::cout << "GLSL " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;
    std::cout << "USD " << PXR_VERSION << std::endl;
    std::cout << "Hydra " << (UsdImagingGLEngine::IsHydraEnabled() ? "enabled" : "disabled") << std::endl;
#if (__APPLE__ && PXR_VERSION < 2208)
    std::cout << "Viewport is disabled on Apple platform with USD < 22.08" << std::endl;
#endif
    const char *pluginPathName = std::getenv("PXR_PLUGINPATH_NAME");
    std::cout << "PXR_PLUGINPATH_NAME: " << (pluginPathName ? pluginPathName : "") << std::endl;

    // Init ImGui for glfw and opengl
    ImGuiContext *mainUIContext = ImGui::GetCurrentContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init();
    // Init an imgui context for hydra, to render the gizmo and hud
    ImGuiContext *hydraUIContext = ImGui::CreateContext();
    ImGui::SetCurrentContext(hydraUIContext);
    ImGui_ImplOpenGL3_Init();

    { // we use a scope as the editor should be deleted before imgui and glfw, to release correctly the memory
        ImGui::SetCurrentContext(mainUIContext);
        Editor editor;

        // Connect the window callbacks to the editor
        editor.InstallCallbacks(window);

        // Process command line options
        for (auto &stage : options.stages()) {
            editor.OpenStage(stage);
        }

        // Loop until the user closes the window
        while (!editor.IsShutdown()) {

            // Poll and process events
            glfwMakeContextCurrent(window);
            glfwPollEvents();

            // Render the viewports first as textures
            ImGui::SetCurrentContext(hydraUIContext);
            editor.HydraRender(); // RenderViewports

            // Render GUI next
            ImGui::SetCurrentContext(mainUIContext);
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            editor.Draw();
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#ifndef DISABLE_DOUBLE_BUFFER
            // Swap front and back buffers
            glfwSwapBuffers(window);
#else
            glFlush();
#endif
            // This forces to wait for the gpu commands to finish.
            // Normally not required but it fixes a pcoip driver issue
            glFinish();

            // Process edition commands
            ExecuteCommands();
        }
        editor.RemoveCallbacks(window);
    }
    ImGui::DestroyContext(hydraUIContext);

    // Shutdown imgui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();

    // Shutdown glfw
    glfwDestroyWindow(window);
    glfwTerminate();

#ifdef WANTS_PYTHON
    Py_Finalize();
#endif

    return 0;
}
