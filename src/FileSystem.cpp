#include "FileSystem.h"

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
extern char **environ;
#endif

bool OpenInFileManager(const fs::path &directory) {
    if (directory.empty()) {
        return false;
    }
#ifdef _WIN32
    // ShellExecute returns a value greater than 32 on success
    const auto result = ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
#else
#ifdef __APPLE__
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    // The path is passed as a separate argument and never goes through a shell, so spaces and
    // shell characters in it are not interpreted
    std::string path = directory.native();
    char *argv[] = {const_cast<char *>(opener), &path[0], nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) != 0) {
        std::cerr << "unable to run " << opener << std::endl;
        return false;
    }
    // Reap the child without blocking the UI, xdg-open can wait for the launched application
    std::thread([pid]() {
        int status = 0;
        waitpid(pid, &status, 0);
    }).detach();
    return true;
#endif
}
