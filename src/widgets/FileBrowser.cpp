/// File browser
/// This is a first quick and dirty implementation,
/// it should be improved to avoid using globals,
/// split the ui and filesystem code, remove the polling timer, etc.

#include <iostream>
#include <functional>
#include <ctime>
#include <algorithm>
#include <chrono>

#include "FileSystem.h"
#include "FileBrowser.h"
#include "Constants.h"
#include "ImGuiHelpers.h"
#include "Gui.h"

namespace clk = std::chrono;

// Convert different time representation to time_t
template <typename TimePointT> std::time_t toTimet(TimePointT timePoint) {
    const auto sysClockTimePoint =
        clk::time_point_cast<clk::system_clock::duration>(timePoint - TimePointT::clock::now() + clk::system_clock::now());
    return clk::system_clock::to_time_t(sysClockTimePoint);
}

using DrivesListT = std::vector<std::pair<std::string, std::string>>;

#ifdef _WIN64
/*
Windows specific implementation
*/
#include <Fileapi.h>
#include <cstdlib> // wcstombs

std::string GetVolumeName(const std::wstring &currentDrive) {
    // Querying the name of a slow or disconnected network share blocks, and this runs at startup
    if (GetDriveTypeW(currentDrive.c_str()) == DRIVE_REMOTE) {
        return "Network drive";
    }
    constexpr size_t volumeNameMaxSize = 512;
    WCHAR volumeName[volumeNameMaxSize];
    volumeName[0] = L'\0';
    GetVolumeInformationW(currentDrive.c_str(), volumeName, volumeNameMaxSize, nullptr, nullptr, nullptr, nullptr, 0);
    if (volumeName[0] != 0) {
        // UTF-8 as expected by ImGui
        char volumeNameMultiBytes[volumeNameMaxSize * 4];
        const int written = WideCharToMultiByte(CP_UTF8, 0, volumeName, -1, volumeNameMultiBytes,
                                                static_cast<int>(sizeof(volumeNameMultiBytes)), nullptr, nullptr);
        if (written > 0) {
            return volumeNameMultiBytes;
        }
    }
    return "Local disk";
}

DrivesListT InitDrivesList() {
    std::setlocale(LC_ALL, "en_US.utf8");
    constexpr size_t drivesListBufferSize = 256;
    wchar_t drivesList[drivesListBufferSize];
    DrivesListT drivesVector;
    DWORD totalSize = GetLogicalDriveStringsW(drivesListBufferSize, (LPWSTR)drivesList);
    std::wstring currentDrive;
    std::string volumeNameStr;
    for (DWORD i = 0; i < totalSize; ++i) {
        if (drivesList[i] != '\0') {
            currentDrive.push_back(drivesList[i]);
        } else {
            drivesVector.emplace_back(
                std::string(
                    currentDrive.begin(),
                    currentDrive.end()), // conversion works because we expect just a drive letter, otherwise this is incorrect
                GetVolumeName(currentDrive.c_str()));
            currentDrive = L"";
        }
    }
    return drivesVector;
}

// TODO: FIX if a new drive is added, it won't be taken into account;
static DrivesListT drivesList = InitDrivesList();

// Calling a thread safe localtime function instead of std::locatime
// (even if the overall code is not thread safe)
inline void localtime_(struct tm *result, const time_t *timep) { localtime_s(result, timep); }

#else
/*
Posix specific implementation
*/
static DrivesListT drivesList;

inline void localtime_(struct tm *result, const time_t *timep) { localtime_r(timep, result); }

#endif

namespace {
/// Browser returned file path, not thread safe
static std::string filePath;
static bool fileExists = false;
static std::vector<std::string> validExts;
static std::string lineEditBuffer;
static fs::path displayedDirectory = CurrentPath(); // fs::current_path() would throw before main
// Reason why the displayed directory could not be listed, shown in place of the list
static std::string directoryError;
};

/// A directory entry with the information needed to sort and draw it, queried once when the directory is listed
struct FileBrowserEntry {
    fs::path path;
    std::string name;
    bool isDirectory = false;
    std::string modified; // empty when unknown
    std::string size;     // empty for directories, special files or when unknown
};

void SetValidExtensions(const std::vector<std::string> &extensions) { validExts = extensions; }

// Using a timer to avoid querying the filesytem at every frame
static void EverySecond(const std::function<void()> &deferedFunction) {
    static auto last = clk::steady_clock::now();
    auto now = clk::steady_clock::now();
    if ((now - last).count() > 1e9) {
        deferedFunction();
        last = now;
    }
}

constexpr char preferred_separator_char_windows = '\\';
constexpr char preferred_separator_char_unix = '/';

// Taken from the filesystem library rather than GHC_OS_WINDOWS, which is only defined with ghc::filesystem
static constexpr char preferred_separator_char = static_cast<char>(fs::path::preferred_separator);

// Paths coming from USD or typed by the user can mix '/' and '\' on Windows, fs::path does not normalize them
static constexpr const char *separator_chars = preferred_separator_char == preferred_separator_char_windows ? "\\/" : "/";

static bool DrawNavigationBar(fs::path &displayedDirectory) {
    // Split the path navigator ??
    std::string lineEditBuffer;
    ScopedStyleColor style(ImGuiCol_Button, ImVec4(ColorTransparent));
    const std::string directoryPath = ToUtf8(displayedDirectory);
    std::string::size_type pos = 0;
    std::string::size_type len = 0;
    len = directoryPath.find_first_of(separator_chars, pos);
    int widgetID = 0;
    while (len != std::string::npos) {
        if (pos == 0 && !drivesList.empty()) {
            if (ImGui::Button(ICON_FA_HDD)) {
                ImGui::OpenPopup("driveslist");
            }
            if (ImGui::BeginPopup("driveslist")) {
                for (auto driveInfo: drivesList) {
                    const std::string& driveLetter = driveInfo.first;
                    const std::string& driveVolumeName = driveInfo.second;
                    const std::string driveText = driveVolumeName + " (" + driveLetter + ")";
                    if (ImGui::Selectable(driveText.c_str(), false)) {
                        lineEditBuffer = driveLetter + preferred_separator_char;
                        displayedDirectory = FromUtf8(lineEditBuffer);
                        ImGui::EndPopup();
                        return true;
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine();
        }
        ImGui::PushID(widgetID++);
        const std::string dirLabel = directoryPath.substr(pos, len - pos);
        if (ImGui::Button(dirLabel.empty() ? "###emptydirlabel" : dirLabel.c_str())) {
            lineEditBuffer = directoryPath.substr(0, len) + preferred_separator_char;
            displayedDirectory = FromUtf8(lineEditBuffer);
            ImGui::PopID();
            return true;
        }
        pos = len + 1;
        len = directoryPath.find_first_of(separator_chars, pos);
        ImGui::SameLine();
        ImGui::Text(">");
        ImGui::SameLine();
        ImGui::PopID();
    }
    len = directoryPath.size();
    const std::string dirLabel = directoryPath.substr(pos, len - pos);
    ImGui::Button(dirLabel.empty() ? "###emptydir" : dirLabel.c_str());
    return false;
}

inline
static bool DrawRefreshButton() {
    ScopedStyleColor style(ImGuiCol_Button, ImVec4(ColorTransparent));
    if (ImGui::Button(ICON_FA_SYNC_ALT)) {
        return true;
    }
    return false;
}

static inline bool FileNameStartsWithDot(const fs::path &path) {
    const auto filename = path.filename().native(); // no conversion, so nothing can throw on Windows
    return filename.size() > 0 && filename[0] == '.';
}

static bool ShouldBeDisplayed(const fs::directory_entry &p) {
    const auto &filename = p.path().filename();
    const auto startsWithDot = FileNameStartsWithDot(p.path());
    bool endsWithValidExt = true;
    if (!validExts.empty()) {
        endsWithValidExt = false;
        for (auto ext : validExts) { // we could stop the loop when the extension is found
            endsWithValidExt |= filename.extension() == ext;
        }
    }

    std::error_code ec;
    const bool isDirectory = p.is_directory(ec);
    if (ec) {
        return false;
    }
    const bool isSymlink = p.is_symlink(ec);
    if (ec) {
        return false;
    }
    return !startsWithDot && (isDirectory || endsWithValidExt) && !isSymlink;
}

static std::string FileSizeText(uintmax_t fileSize) {
    static const char *format[6] = {"%juB", "%juK", "%juM", "%juG", "%juT", "%juP"};
    constexpr int nbFormat = sizeof(format) / sizeof(const char *);
    int i = 0;
    while (fileSize / 1024 > 0 && i < nbFormat - 1) {
        fileSize /= 1024;
        i++;
    }
    char text[32];
    snprintf(text, sizeof(text), format[i], fileSize);
    return text;
}

static std::string ModifiedTimeText(const fs::directory_entry &dirEntry) {
    std::error_code ec;
    const auto lastModified = dirEntry.last_write_time(ec);
    if (ec) {
        return {};
    }
    const time_t cftime = toTimet(lastModified);
    struct tm lt; // Convert to local time
    localtime_(&lt, &cftime);
    char text[32];
    snprintf(text, sizeof(text), "%04d/%02d/%02d %02d:%02d", 1900 + lt.tm_year, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour,
             lt.tm_min);
    return text;
}

// Only regular files have a size, file_size fails on sockets, pipes and devices
static FileBrowserEntry MakeFileBrowserEntry(const fs::directory_entry &dirEntry) {
    FileBrowserEntry entry;
    std::error_code ec;
    entry.path = dirEntry.path();
    entry.name = ToUtf8(entry.path.filename());
    entry.isDirectory = dirEntry.is_directory(ec);
    entry.modified = ModifiedTimeText(dirEntry);
    if (!entry.isDirectory && dirEntry.is_regular_file(ec)) {
        const uintmax_t fileSize = dirEntry.file_size(ec);
        if (!ec) {
            entry.size = FileSizeText(fileSize);
        }
    }
    return entry;
}

// Compare function for sorting directories before files
static bool compareDirectoryThenFile(const FileBrowserEntry &a, const FileBrowserEntry &b) {
    if (a.isDirectory == b.isDirectory) {
        return a.path < b.path;
    } else {
        return a.isDirectory > b.isDirectory;
    }
}

void DrawFileBrowser(int height) {

    static fs::path displayedFileName;
    static std::vector<FileBrowserEntry> directoryContent;
    static fs::path directoryContentPath;
    static bool mustUpdateDirectoryContent = true;
    static bool mustUpdateChosenFileName = false;

    // Parse the line buffer containing the user input and try to make sense of it
    auto ParseLineBufferEdit = [&]() {
        auto path = FromUtf8(lineEditBuffer);
        if (path != path.root_name() && IsDirectory(path)) {
            displayedDirectory = path;
            mustUpdateDirectoryContent = true;
            lineEditBuffer = "";
            displayedFileName = "";
            mustUpdateChosenFileName = true;
        } else if (path.parent_path() != path.root_name() && IsDirectory(path.parent_path())) {
            displayedDirectory = path.parent_path();
            mustUpdateDirectoryContent = true;
            lineEditBuffer = ToUtf8(path.filename());
            displayedFileName = path.filename();
            mustUpdateChosenFileName = true;
        } else {
            if (path.filename() != path) {
                displayedFileName = path.filename();
                mustUpdateChosenFileName = true;
                lineEditBuffer = ToUtf8(path.filename());
            } else {
                displayedFileName = path;
                mustUpdateChosenFileName = true;
            }
        }
    };

    // Update the list of entries for the chosen directory
    auto UpdateDirectoryContent = [&]() {
        directoryContent.clear();
        directoryError.clear();
        // Explicit increment(ec) as the range-for operator++ throws on error
        std::error_code ec;
        fs::directory_iterator it(displayedDirectory, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
            if (ShouldBeDisplayed(*it)) {
                directoryContent.push_back(MakeFileBrowserEntry(*it));
            }
        }
        if (ec) {
            directoryError = ec.message();
        }
        std::sort(directoryContent.begin(), directoryContent.end(), compareDirectoryThenFile);
        mustUpdateDirectoryContent = false;
        directoryContentPath = displayedDirectory;
    };

    if (mustUpdateChosenFileName) {
        if (!displayedDirectory.empty() && !displayedFileName.empty() && IsDirectory(displayedDirectory)) {
            const auto path_ = displayedDirectory / displayedFileName;
            filePath = ToUtf8(path_);
        } else {
            filePath = "";
        }
        fileExists = !filePath.empty() && PathExists(FromUtf8(filePath));
        mustUpdateChosenFileName = false;
    }

    // We scan the line buffer edit every second, no need to do it at every frame
    EverySecond(ParseLineBufferEdit);
    mustUpdateDirectoryContent |= directoryContentPath != displayedDirectory;
    mustUpdateDirectoryContent |= DrawRefreshButton();
    ImGui::SameLine();
    mustUpdateDirectoryContent |= DrawNavigationBar(displayedDirectory);    height -= ImGui::GetFrameHeight(); // remove the height of the navigation bar
    if (mustUpdateDirectoryContent) {
        UpdateDirectoryContent();
    }

    // Get window size
    //ImGuiWindow *currentWindow = ImGui::GetCurrentWindow();
    
    // We want to compute the height of the file list, so we need to remove the
    // navigation bar height and the filename textbox from the total height
    height -= 2*ImGui::GetFrameHeightWithSpacing(); // remove the height of the navigation bar
    
    ImVec2 sizeArg(-1, height);
    ImGui::PushItemWidth(-1); // List takes the full width
    if (!directoryError.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "Cannot read this folder: %s", directoryError.c_str());
        sizeArg.y -= ImGui::GetTextLineHeightWithSpacing();
    }
    if (ImGui::BeginListBox("##FileList", sizeArg)) {
        constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg
            | ImGuiTableFlags_ScrollY
            | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("Files", 4, tableFlags)) {
            ImGui::TableSetupScrollFreeze(4, 1);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize);
            ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Date modified", ImGuiTableColumnFlags_WidthStretch|ImGuiTableColumnFlags_NoResize);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoResize);
            ImGui::TableHeadersRow();
            int i = 0;
            ImGui::PushID("direntries");
            for (const auto &dirEntry : directoryContent) {
                const bool isDirectory = dirEntry.isDirectory;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i++);
                // makes the line selectable, and when selected copy the path
                // to the line edit buffer
                if (ImGui::Selectable("", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowItemOverlap)) {
                    if (isDirectory) {
                        displayedDirectory = dirEntry.path;
                        mustUpdateDirectoryContent = true;
                    } else {
                        displayedFileName = dirEntry.path;
                        lineEditBuffer = ToUtf8(dirEntry.path);
                        mustUpdateChosenFileName = true;
                    }
                }
                ImGui::PopID();
                ImGui::SameLine();
                if (isDirectory) {
                    ImGui::TextColored(ImVec4(1.0, 1.0, 0.0, 1.0), "%s ", ICON_FA_FOLDER);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(ImVec4(1.0, 1.0, 1.0, 1.0), "%s", dirEntry.name.c_str());
                } else {
                    ImGui::TextColored(ImVec4(0.9, 0.9, 0.9, 1.0), "%s ", ICON_FA_FILE);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextColored(ImVec4(0.5, 1.0, 0.5, 1.0), "%s", dirEntry.name.c_str());
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(dirEntry.modified.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(dirEntry.size.c_str());
            }
            ImGui::PopID(); // direntries
            ImGui::EndTable();
        }
        ImGui::EndListBox();
    }

    ImGui::PopItemWidth();
    ImGui::InputText("File name", &lineEditBuffer);
}

bool FilePathExists() { return fileExists; }

std::string GetFileBrowserFilePath() { return filePath; }

std::string GetFileBrowserDirectory() { return ToUtf8(displayedDirectory); }



std::string GetFileBrowserFilePathRelativeTo(const std::string &root, bool unixify) {
    fs::path rootPath = FromUtf8(root);
    const fs::path originalPath = FromUtf8(filePath);
    if (rootPath.is_absolute() && originalPath.is_absolute()) {
        rootPath.remove_filename();
        std::error_code ec;
        const fs::path relativePath = fs::relative(originalPath, rootPath, ec);
        if (ec) {
            return filePath;
        }
        auto relativePathStr = ToUtf8(relativePath);
        if (unixify) {
            std::replace(relativePathStr.begin(), relativePathStr.end(), preferred_separator_char_windows,
                         preferred_separator_char_unix);
        }
        return relativePathStr;
    }
    return filePath;
}

void EnsureFileBrowserDefaultExtension(const std::string &ext) {
    fs::path path = FromUtf8(filePath);
    if (!path.empty() && !path.has_extension()) {
        path.replace_extension(ext);
        filePath = ToGenericUtf8(path);
        fileExists = PathExists(path);
    }
}

void EnsureFileBrowserExtension(const std::string &ext) {
    fs::path path = FromUtf8(filePath);
    if (!path.empty()) {
        path.replace_extension(ext);
        filePath = ToGenericUtf8(path);
        fileExists = PathExists(path);
    }
}

void ResetFileBrowserFilePath() {
    filePath = "";
    lineEditBuffer = "";
}


void SetFileBrowserDirectory(const std::string &directory) {
    const fs::path directoryPath = FromUtf8(directory);
    if (IsDirectory(directoryPath)) {
        displayedDirectory = directoryPath;
    }
}

void SetFileBrowserFilePath(const std::string &path) {
    lineEditBuffer = path;
}
