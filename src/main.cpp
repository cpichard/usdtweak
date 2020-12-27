#include <iostream>
#include <Python.h>
#include <pxr/base/plug/registry.h>
#include <pxr/imaging/glf/contextCaps.h>
#include <pxr/imaging/glf/glew.h>
#include <pxr/imaging/glf/simpleLight.h>
#include <pxr/imaging/glf/diagnostic.h>
//#define GLFW_INCLUDE_GLCOREARB
#include <GLFW/glfw3.h>
#include "Gui.h"
#include "Editor.h"
#include "Viewport.h"
#include "Commands.h"
#include "Constants.h"

PXR_NAMESPACE_USING_DIRECTIVE

int main(int argc, char **argv) {

    // Initialize python
    // TODO: look for folder and hooks
    Py_SetProgramName(argv[0]);
    Py_Initialize();

    // Initialize glfw
    if (!glfwInit())
        return -1;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
    // glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE); // MacOS

    /* Create a windowed mode window and its OpenGL context */
    int width = InitialWindowWidth;
    int height = InitialWindowHeight;
    GLFWwindow *window = glfwCreateWindow(width, height, "USD Tweak", NULL, NULL);
    if (!window) {
        std::cerr << "unable to create a window, exiting" << std::endl;
        glfwTerminate();
        return -1;
    }

    // Make the window's context current
    glfwMakeContextCurrent(window);

    // Init glew with USD
    GlfGlewInit();
    GlfContextCaps::InitInstance();
    std::cout << glGetString(GL_VENDOR) << std::endl;
    std::cout << glGetString(GL_RENDERER) << std::endl;
    std::cout << glGetString(GL_VERSION) << std::endl;
    std::cout << "GLSL " << glGetString(GL_SHADING_LANGUAGE_VERSION) << std::endl;
    std::cout << "Hydra enabled : " << UsdImagingGLEngine::IsHydraEnabled() << std::endl;
    // GlfRegisterDefaultDebugOutputMessageCallback();

    // Create a
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init();
    auto font_default = io.Fonts->AddFontDefault();

    // Dock with shift key
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigDockingWithShift = true;
    io.ConfigWindowsMoveFromTitleBarOnly = true;

    { // Scope as the editor should be deleted before imgui and glfw, to release correctly the memory
        Editor editor;
        glfwSetWindowUserPointer(window, &editor);
        glfwSetDropCallback(window, Editor::DropCallback);

        // Loop until the user closes the window
        while (!glfwWindowShouldClose(window) && !editor.ShutdownRequested()) {

            // Poll and process events
            glfwMakeContextCurrent(window);
            glfwPollEvents();

            // Render the viewports first as textures
            editor.HydraRender();

            // Render GUI next
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            editor.Draw();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            // Swap front and back buffers
            glfwSwapBuffers(window);

            // Process edition commands
            ExecuteCommands();
        }

        glfwSetWindowUserPointer(window, nullptr);
    }

    // Shutdown imgui
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    // Shutdown glfw
    glfwDestroyWindow(window);
    glfwTerminate();
    Py_Finalize();
    return 0;
}
