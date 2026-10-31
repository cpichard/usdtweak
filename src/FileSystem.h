#pragma once
/// Filesystem include shared by the application, and helpers that never throw.
/// Most std::filesystem calls throw on any error other than "not found" (permission denied, symlink loop,
/// name too long, deleted working directory...). An uncaught exception terminates usdtweak, so code
/// outside a try block should use these helpers or the std::error_code overloads.

#include <string>
#include <system_error>

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

// Non throwing versions of fs::exists, fs::is_directory and fs::is_regular_file, a filesystem error is
// treated as false
inline bool PathExists(const fs::path &path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

inline bool IsDirectory(const fs::path &path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

inline bool IsRegularFile(const fs::path &path) {
    std::error_code ec;
    return fs::is_regular_file(path, ec);
}

// Paths are exchanged as UTF-8 strings, like ImGui and USD. path::string() would use the Windows ANSI
// code page and throw on names it can't represent, and fs::path(std::string) would misread UTF-8.
// A name that can't be converted at all is returned as "?".
// Note: u8string returns std::string up to C++17, std::u8string from C++20.
inline std::string ToUtf8(const fs::path &path) {
    try {
        return path.u8string();
    } catch (const std::exception &) {
        return "?";
    }
}

inline std::string ToGenericUtf8(const fs::path &path) {
    try {
        return path.generic_u8string();
    } catch (const std::exception &) {
        return "?";
    }
}

inline fs::path FromUtf8(const std::string &text) {
    try {
        return fs::u8path(text);
    } catch (const std::exception &) {
        return {};
    }
}

// Opens the directory in the platform file manager (Finder, Explorer, or xdg-open on Linux) without
// going through a shell. Returns false if the file manager couldn't be started.
bool OpenInFileManager(const fs::path &directory);

// The working directory, or an empty path when it can't be read (deleted directory)
inline fs::path CurrentPath() {
    std::error_code ec;
    const fs::path path = fs::current_path(ec);
    return ec ? fs::path() : path;
}
