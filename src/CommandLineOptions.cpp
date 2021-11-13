#include "CommandLineOptions.h"

CommandLineOptions::CommandLineOptions(int argc, const char **argv) {
    for (int i = 1; i < argc; ++i) {
        _stages.push_back(argv[i]);
    }
}