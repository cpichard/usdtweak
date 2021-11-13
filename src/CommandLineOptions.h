#include <vector>
#include <string>

class CommandLineOptions {
  public:
    CommandLineOptions(int argc, const char **argv);

    const std::vector<std::string> &stages() { return _stages; }

  private:
    std::vector<std::string> _stages;
};