/// File browser
/// This is a first quick and dirty implementation,
/// it should be improved to avoid using globals,
/// split the ui and filesystem code, remove the polling timer, etc.

#include <iostream>
#include <functional>
#include <ctime>
#include <chrono>

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

#include "FileBrowser.h"
#include "Constants.h"
#include "ImGuiHelpers.h"
#include "Gui.h"

namespace clk = std::chrono;

#ifdef _WIN64
/*
Windows specific implementation
*/
#include <Fileapi.h>
std::vector<std::string> InitDrivesList() {
    constexpr size_t drivesListBufferSize = 256;
    wchar_t drivesList[drivesListBufferSize];
    std::vector<std::string> drivesVector;
    DWORD totalSize = GetLogicalDriveStringsW(drivesListBufferSize, (LPWSTR)drivesList);
    std::wstring currentDrive;
    for (DWORD i = 0; i < totalSize; ++i) {
        if (drivesList[i] != '\0') {
            currentDrive.push_back(drivesList[i]);
        } else {
            drivesVector.emplace_back(currentDrive.begin(), currentDrive.end());
            currentDrive = L"";
        }
    }
    return drivesVector;
}

static std::vector<std::string> drivesList = InitDrivesList();

// Calling a thread safe localtime function instead of std::locatime
// (even if the overall code is not thread safe)
inline void localtime_(struct tm *result, const time_t *timep) { localtime_s(result, timep); }

#else
/*
Posix specific implementation
*/
static std::vector<std::string> drivesList;

inline void localtime_(struct tm *result, const time_t *timep) { localtime_r(timep, result); }

#endif

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

inline void ConvertToDirectory(const fs::path &path, std::string &directory) {
    if (!path.empty() && path == path.root_name()) { // this is a drive
        directory = path / path.root_directory();
    } else {
        directory = path;
    }
}

static bool DrawNavigationBar(fs::path &displayedDirectory) {
    // Split the path navigator ??
    std::string lineEditBuffer;
    ScopedStyleColor style(ImGuiCol_Button, ImVec4(TransparentColor));
    const std::string &directoryPath = displayedDirectory.string();
    std::string::size_type pos = 0;
    std::string::size_type len = 0;
    len = directoryPath.find(fs::path::preferred_separator, pos);

    while (len != std::string::npos) {
        if (ImGui::Button(directoryPath.substr(pos, len - pos).c_str())) {
            lineEditBuffer = directoryPath.substr(0, len) + fs::path::preferred_separator;
            displayedDirectory = fs::path(lineEditBuffer);
            return true;
        }
        if (pos == 0 && !drivesList.empty()) { // Show the drives if there are any in the list
            if (ImGui::BeginPopupContextItem()) {
                for (auto driveLetter : drivesList) {
                    if (ImGui::Selectable(driveLetter.c_str(), false)) {
                        lineEditBuffer = driveLetter + fs::path::preferred_separator;
                        displayedDirectory = fs::path(lineEditBuffer);
                        ImGui::EndPopup();
                        return true;
                    }
                }
                ImGui::EndPopup();
            }
        }
        pos = len + 1;
        len = directoryPath.find(fs::path::preferred_separator, pos);
        ImGui::SameLine();
        ImGui::Text(">");
        ImGui::SameLine();
    }
    len = directoryPath.size();
    ImGui::Button(directoryPath.substr(pos, len - pos).c_str());
    return false;
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
        return !startsWithDot && (isDirectory || endsWithValidExt) && !fs::is_symlink(p);
    } catch (fs::filesystem_error &) {
        return false;
    }
}

// Compare function for sorting directories before files
static bool compareDirectoryThenFile(const fs::directory_entry &a, const fs::directory_entry &b) {
    if (fs::is_directory(a) == fs::is_directory(b)) {
        return a < b;
    } else {
        return fs::is_directory(a) > fs::is_directory(b);
    }
}

static void DrawFileSize(uintmax_t fileSize) {
    static const char *format[6] = {"%juB", "%juK", "%juM", "%juG", "%juT", "%juP"};
    constexpr int nbFormat = sizeof(format) / sizeof(const char *);
    int i = 0;
    while (fileSize / 1024 > 0 && i < nbFormat - 1) {
        fileSize /= 1024;
        i++;
    }
    ImGui::Text(format[i], fileSize);
}

void DrawFileBrowser() {
    static std::string lineEditBuffer;
    static fs::path displayedDirectory = fs::current_path();
    static fs::path displayedFileName;
    static std::vector<fs::directory_entry> directoryContent;
    static bool mustUpdateDirectoryContent = true;
    static bool mustUpdateChosenFileName = false;

    // Parse the line buffer containing the user input and try to make sense of it
    auto ParseLineBufferEdit = [&]() {
        auto path = fs::path(lineEditBuffer);
        if (path != path.root_name() && fs::is_directory(path)) {
            displayedDirectory = path;
            mustUpdateDirectoryContent = true;
            lineEditBuffer = "";
            displayedFileName = "";
            mustUpdateChosenFileName = true;
        } else if (path.parent_path() != path.root_name() && fs::is_directory(path.parent_path())) {
            displayedDirectory = path.parent_path();
            mustUpdateDirectoryContent = true;
            lineEditBuffer = path.filename().string();
            displayedFileName = path.filename();
            mustUpdateChosenFileName = true;
        } else {
            if (path.filename() != path) {
                displayedFileName = path.filename();
                mustUpdateChosenFileName = true;
                lineEditBuffer = path.filename().string();
            } else {
                displayedFileName = path;
                mustUpdateChosenFileName = true;
            }
        }
    };

    // Update the list of entries for the chosen directory
    auto UpdateDirectoryContent = [&]() {
        directoryContent.clear();
        for (auto &item : fs::directory_iterator(displayedDirectory)) {
            if (ShouldBeDisplayed(item)) {
                directoryContent.push_back(item);
            };
        }
        std::sort(directoryContent.begin(), directoryContent.end(), compareDirectoryThenFile);
        mustUpdateDirectoryContent = false;
    };

    if (mustUpdateChosenFileName) {
        if (!displayedDirectory.empty() && !displayedFileName.empty() && fs::exists(displayedDirectory) &&
            fs::is_directory(displayedDirectory)) {
            filePath = displayedDirectory / displayedFileName;
        } else {
            filePath = "";
        }
        fileExists = fs::exists(filePath);
        mustUpdateChosenFileName = false;
    }

    // We scan the line buffer edit every second, no need to do it at every frame
    EverySecond(ParseLineBufferEdit);

    if (mustUpdateDirectoryContent) {
        UpdateDirectoryContent();
    }

    ImGui::PushItemWidth(-1); // List takes the full size

    mustUpdateDirectoryContent |= DrawNavigationBar(displayedDirectory);

    // Get window size
    ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
    ImVec2 sizeArg(0, currentWindow->Size[1] - 170); // TODO: 170 should be computer

    if (ImGui::BeginListBox("##FileList", sizeArg)) {
        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg;
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
                    if (isDirectory) {
                        displayedDirectory = dirEntry.path();
                        mustUpdateDirectoryContent = true;
                    } else {
                        displayedFileName = dirEntry.path();
                        lineEditBuffer = dirEntry.path().string();
                        mustUpdateChosenFileName = true;
                    }
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
                    const time_t cftime = decltype(lastModified)::clock::to_time_t(lastModified);
                    struct tm lt; // Convert to local time
                    localtime_(&lt, &cftime);
                    ImGui::Text("%04d/%02d/%02d %02d:%02d", 1900 + lt.tm_year, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min);
                } catch (const std::exception &) {
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
    ImGui::InputText("File name", &lineEditBuffer);
}

bool FilePathExists() { return fileExists; }

std::string GetFileBrowserFilePath() { return filePath; }