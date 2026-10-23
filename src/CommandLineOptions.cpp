#include "CommandLineOptions.h"

#include "FileSystem.h"

CommandLineOptions::CommandLineOptions(int argc, char *const *argv) {
    for (int i = 1; i < argc; ++i) {
        // argv uses the native narrow encoding (the ANSI code page on Windows), hence fs::path and not FromUtf8.
        // fs::absolute fails when the working directory was deleted, the argument is then kept as given.
        const fs::path argument(argv[i]);
        std::error_code ec;
        const fs::path absolutePath = fs::absolute(argument, ec);
        _stages.push_back(ToUtf8(ec ? argument : absolutePath));
    }
}
