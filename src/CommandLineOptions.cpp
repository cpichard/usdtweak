#include "CommandLineOptions.h"

CommandLineOptions::CommandLineOptions(int argc, char *const *argv) {
    for (int i = 1; i < argc; ++i) {
        _stages.push_back(argv[i]);
    }
}
