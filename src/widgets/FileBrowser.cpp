/// File browser
/// This is a first quick and dirty implementation,
/// it should be improved to avoid using globals,
/// split the ui and filesystem code, remove the polling timer, etc.

#include <iostream>
#include <functional>
#include <ctime>
#include <chrono>
#include "Gui.h"
#include "FileBrowser.h"
#include "Constants.h"
#include "ImGuiHelpers.h"

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

void SetValidExtensions(const std::vector<std::string> &extensions) { validExts = extensions; }

// Using a timer to avoid querying the filesytem at every frame
// TODO: a separate thread to read from the filesystem only once needed as it might take
// more than one second to return the list of files on network drives
// or things like inotify ?? and the equivalent on linux ?
static void EverySecond(const std::function<void()> &deferedFunction) {
    static auto last = clk::steady_clock::now();
    auto now = clk::steady_clock::now();
    if ((now - last).count() > 1e9) {
        deferedFunction();
        last = now;
    }
}

static void ClearPathBuffer(char *pathBuffer) { memset(pathBuffer, 0, PathBufferSize); }
//
/// Path buffer must have PathBufferSize
///
/// TODO: use imgui_stdlib instead of converting string to fixed char array
static void CopyToPathBuffer(const std::string &src, char *pathBuffer) {
    const int n = std::min(PathBufferSize - 1, src.size());
    ClearPathBuffer(pathBuffer);
    strncpy(&pathBuffer[0], src.c_str(), n);
}

static void DrawNavigationBar(const fs::path &displayedDirectory, char *lineEditBuffer) {
    // Split the path navigator ??
    ScopedStyleColor style(ImGuiCol_Button, ImVec4(TransparentColor));
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
        ImGui::Text(">");
        ImGui::SameLine();
    }
    len = directoryPath.size();
    if (ImGui::Button(directoryPath.substr(pos, len - pos).c_str())) {
        CopyToPathBuffer(directoryPath.substr(0, len), lineEditBuffer);
    }
}

static bool ShouldBeDisplayed(const fs::directory_entry &p) {
    const auto &filename = p.path().filename();
    // TODO: startsWith is defined in ghc/filesystem.hpp and won't compile with c++17
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
    } catch (fs::filesystem_error &) {
        return false;
    }
}

// Compare function for sorting directories before files
static bool directoryThenFile(const fs::directory_entry &a, const fs::directory_entry &b) {
    if (fs::is_directory(a) == fs::is_directory(b)) {
        return a < b;
    } else {
        return fs::is_directory(a) > fs::is_directory(b);
    }
}

void DrawFileSize(uintmax_t fileSize) {
    static const char *format[6] = {"%ju", "%juKb", "%juMb", "%juGb", "%juTb", "%juPb"};
    constexpr int nbFormat = sizeof(format) / sizeof(const char *);
    int i = 0;
    while (fileSize / 1024 > 0 && i < nbFormat - 1) {
        fileSize /= 1024;
        i++;
    }
    ImGui::Text(format[i], fileSize);
}

// TODO check that there is a antislash/slash at the end of c
void DrawFileBrowser() {
    static char lineEditBuffer[PathBufferSize]; // TODO: check if it can be replaced by imgui_stdlib
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
        if (!displayedDirectory.empty() && !displayedFileName.empty() && fs::exists(displayedDirectory) &&
            fs::is_directory(displayedDirectory)) {
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
        std::sort(directoryContent.begin(), directoryContent.end(), directoryThenFile);
    });

    ImGui::PushItemWidth(-1); // List takes the full size

    DrawNavigationBar(displayedDirectory, lineEditBuffer);

    static ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg;
    // Get window size
    ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
    ImVec2 sizeArg(0, currentWindow->Size[1] - 170); // TODO: size of the window
    if (ImGui::BeginListBox("##FileList", sizeArg)) {
        if (ImGui::BeginTable("Files", 4, tableFlags)) {
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Date modified", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            int i = 0;
            ImGui::PushID("direntries");
            for (auto dirEntry : directoryContent) {
                const bool isDirectory = dirEntry.is_directory();
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i++);
                // makes the line selectable, and when selected copy the path
                // to the line edit buffer
                if (ImGui::Selectable("", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
                    CopyToPathBuffer(dirEntry.path().string(), lineEditBuffer);
                }
                ImGui::PopID();
                ImGui::SameLine();
                if (isDirectory) {
                    ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "%s ", ICON_FA_FOLDER);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(ImVec4(1.0, 1.0, 1.0, 1.0), "%s", dirEntry.path().filename().c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.9, 0.9, 0.9, 1.0), "%s ", ICON_FA_FILE);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(ImVec4(0.5, 1.0, 0.5, 1.0), "%s", dirEntry.path().filename().c_str());
                }
                ImGui::TableSetColumnIndex(2);
                try {
                    const auto lastModified = last_write_time(dirEntry.path());
                    const auto cftime = decltype(lastModified)::clock::to_time_t(lastModified);
                    const auto lt = std::localtime(&cftime);
                    ImGui::Text("%04d/%02d/%02d %02d:%02d", 1900 + lt->tm_year, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour,
                                lt->tm_min);
                } catch (const std::exception &e) {
                    ImGui::Text("Error reading file");
                }
                ImGui::TableSetColumnIndex(3);
                DrawFileSize(dirEntry.file_size());
            }
            ImGui::PopID(); // direntries
            ImGui::EndTable();
        }
        ImGui::EndListBox();
    }

    ImGui::PushItemWidth(0);
    ImGui::InputText("File name", lineEditBuffer, PathBufferSize);
}

bool FilePathExists() { return fileExists; }

std::string GetFileBrowserFilePath() { return filePath; }
