/// File browser
/// This is a first quick implementation,
/// it should be refactored to avoid using globals,
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
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

namespace clk = std::chrono;

/// Browser returned file path, not thread safe
static std::string filePath;
static bool fileExists = false;
static std::vector<std::string> validExts;


void SetValidExtensions(const std::vector<std::string> &extensions) {
    validExts = extensions;
}

// Using a timer to avoid querying the filesytem at every frame
// TODO: a separate thread to read from the filesystem only once needed
static void EverySecond(const std::function<void()> &deferedFunction) {
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

static void DrawNavigationBar(const fs::path &displayedDirectory, char *lineEditBuffer) {
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

static bool ShouldBeDisplayed(const fs::directory_entry &p) {
    const auto &filename = p.path().filename();
    // TODO: startsWith doesn't seem to be in the standard c++17, this needs a refactor !
    const auto startsWithDot = fs::detail::startsWith(p.path().filename(), ".");
    bool endsWithValidExt = true;
    if (!validExts.empty()) {
        endsWithValidExt = false;
        for (auto ext : validExts) { // we could stop the loop when the extension is found
            endsWithValidExt |= filename.extension() == ext;
        }
    }

    try {
        const bool isDirectory = fs::is_directory(p);
        return !startsWithDot && (isDirectory || endsWithValidExt);
    }
    catch (fs::filesystem_error &) {
        return false;
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
        if (fs::is_directory(path)) {
            displayedDirectory = path;
            ClearPathBuffer(lineEditBuffer);
            displayedFileName = "";
        } else if (fs::is_directory(path.parent_path())) {
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
            if (ShouldBeDisplayed(p)) {
                directoryContent.push_back(p);
            };
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
    if (ImGui::ListBoxHeader("##FileList", sizeArg)) {
        if (ImGui::BeginTable("Files", 3, tableFlags)) {
            ImGui::TableSetupColumn(" Type ", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn(" Filename ", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableAutoHeaders();
            int i = 0;
            ImGui::PushID("direntries");
            for (auto dirEntry : directoryContent) {
                const bool isDirectory = dirEntry.is_directory();
                ImGui::TableNextRow();
                ImGui::PushID(i++);
                // makes the line selectable, and when selected copy the path
                // to the line edit buffer
                if (ImGui::Selectable("", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
                    CopyToPathBuffer(dirEntry.path().string(), lineEditBuffer);
                }
                ImGui::PopID();
                ImGui::SameLine();
                ImGui::Text("%s ", isDirectory ? "D" : "F");
                ImGui::TableNextCell();
                if (isDirectory) {
                    ImGui::TextColored(ImVec4(1.0, 1.0, 1.0, 1.0), "%s", dirEntry.path().filename().c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.8, 1.0, 0.8, 1.0), "%s", dirEntry.path().filename().c_str());
                }
                ImGui::TableNextCell();
                if (!isDirectory) ImGui::Text("%ju KB", dirEntry.file_size()/1024);
            }
            ImGui::PopID(); // direntries
            ImGui::EndTable();
        }
        ImGui::ListBoxFooter();
    }

    ImGui::PushItemWidth(0);
    ImGui::InputText("File name", lineEditBuffer, PathBufferSize);
}

bool FilePathExists() { return fileExists; }

std::string GetFileBrowserFilePath() { return filePath; }
