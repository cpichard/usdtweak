/// File browser
/// This is a first rough implementation, it should be refactored to avoid using globals,
/// split the ui and filesystem code, remove the timer, etc.
#include <iostream>
#include <functional>
#include <chrono>
#include <vector>
#include <string>
#include "Gui.h"
#include "FileBrowser.h"
#include "Constants.h"

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

namespace clk = std::chrono;

/// Browser returned file path, not thread safe
static std::string filePath;
static bool fileExists = false;

// Using a timer to avoid querying the filesytem at every frame
// TODO: a separate thread to read from the filesystem only once needed
void EverySecond(std::function<void()> deferedFunction) {
    static auto last = clk::steady_clock::now();
    auto now = clk::steady_clock::now();
    if ((now - last).count() > 1e9) {
        deferedFunction();
        last = now;
    }
}

static void ClearPathBuffer(char *pathBuffer){
    memset(pathBuffer, 0, PathBufferSize);
}
//
/// Path buffer must have PathBufferSize
static void CopyToPathBuffer(const std::string &src, char *pathBuffer) {
    const int n = std::min(PathBufferSize-1, src.size());
    ClearPathBuffer(pathBuffer);
    strncpy(&pathBuffer[0], src.c_str(), n);
}

static void DrawNavigationBar(fs::path &displayedDirectory, char *lineEditBuffer) {
    // Split the path navigator ??
    const std::string &directoryPath = displayedDirectory.string();
    std::string::size_type pos = 0;
    std::string::size_type len = 0;
    len = directoryPath.find(fs::path::preferred_separator, pos);
    while (len != std::string::npos) {
        if (ImGui::Button(directoryPath.substr(pos, len - pos).c_str())) {
            if (len) {
                CopyToPathBuffer(directoryPath.substr(0, len), lineEditBuffer);
            } else {
                CopyToPathBuffer("/", lineEditBuffer);
            }
        }
        pos = len + 1;
        len = directoryPath.find(fs::path::preferred_separator, pos);
        ImGui::SameLine();
        ImGui::Text("/");
        ImGui::SameLine();
    }
    len = directoryPath.size();
    if (ImGui::Button(directoryPath.substr(pos, len-pos).c_str())) {
        CopyToPathBuffer(directoryPath.substr(0, len), lineEditBuffer);
    }
}


// TODO check that there is a antislash/slash at the end of c:
void DrawFileBrowser() {
    static char lineEditBuffer[PathBufferSize];
    static fs::path displayedDirectory = fs::current_path();
    static fs::path displayedFileName;
    static std::vector<fs::directory_entry> directoryContent;

    // Update the list of directory entries every seconds
    auto path = fs::path(lineEditBuffer);
    EverySecond([&]() {
        // Process the line entered by the user
        if (fs::exists(path) // TODO: not sure it need is_directory and exists
            && fs::is_directory(path)) {
            displayedDirectory = path;
            ClearPathBuffer(lineEditBuffer);
            displayedFileName = "";
        } else if (fs::is_directory(path.parent_path())
            && fs::exists(path.parent_path())) {
            displayedDirectory = path.parent_path();
            CopyToPathBuffer(path.filename().string(), lineEditBuffer);
            displayedFileName = path.filename();
        } else {
            if (path.filename() != path) {
                displayedFileName = path.filename();
                CopyToPathBuffer(path.filename().string(), lineEditBuffer);
            } else {
                displayedFileName = path;
            }
        }
        if (!displayedDirectory.empty()
            && !displayedFileName.empty()
            && fs::exists(displayedDirectory)
            && fs::is_directory(displayedDirectory)) {
            filePath = displayedDirectory / displayedFileName;
        } else {
            filePath = "";
        }

        fileExists = fs::exists(filePath);
        directoryContent.clear();
        for (auto &p : fs::directory_iterator(displayedDirectory)) {
            if (!fs::detail::startsWith(p.path().filename(), ".")) {
                directoryContent.push_back(p);} ;
        }
        std::sort(directoryContent.begin(), directoryContent.end());
    });

    ImGui::PushItemWidth(-1); // List takes the full size

    DrawNavigationBar(displayedDirectory, lineEditBuffer);
    ImGui::Text(" ");

    static ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg;
    // Get window size
    ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
    ImVec2 sizeArg(0, currentWindow->Size[1] - 170); // TODO: size of the window
    if (ImGui::ListBoxHeader("##empty", sizeArg)) {
        if (ImGui::BeginTable("Files", 3, tableFlags)) {
            ImGui::TableSetupColumn(" Type ", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(" Filename ", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableAutoHeaders();
            int i = 0;
            for (auto dirEntry : directoryContent) {
                ImGui::TableNextRow();
                char label[32]; // TODO: is there a better/faster way to create label ?
                sprintf(label, "##filestable_%04d", i++);
                if (ImGui::Selectable(label, false,
                    ImGuiSelectableFlags_SpanAllColumns |
                    ImGuiSelectableFlags_AllowItemOverlap) ){
                        CopyToPathBuffer(dirEntry.path().string(), lineEditBuffer);
                }
                ImGui::SameLine();
                ImGui::Text("%s", dirEntry.is_directory() ? "D " : "F ");
                ImGui::TableNextCell();
                ImGui::Text("%s", dirEntry.path().filename().c_str());
                ImGui::TableNextCell();
                ImGui::Text("%ju", dirEntry.file_size());
            }
            ImGui::EndTable();
        }
        ImGui::ListBoxFooter();
    }

    ImGui::PushItemWidth(0);
    ImGui::InputText("File name", lineEditBuffer, PathBufferSize);
}

bool FilePathExists() { return fileExists; }

std::string GetFileBrowserFilePath() { return filePath; }
