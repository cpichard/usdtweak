#include "CommandLineOptions.h"

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

CommandLineOptions::CommandLineOptions(int argc, char *const *argv) {
    for (int i = 1; i < argc; ++i) {
        _stages.push_back(fs::absolute(fs::path(argv[i])).string());
    }
}
