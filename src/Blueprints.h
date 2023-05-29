#pragma once

#include <string>
#include <unordered_map>
#include <vector>

// Blueprints class
//   - traverse the blueprint root locations looking for layers organised hierarchically on the disk.
//   - keep the hierarchy structure, names and paths of the blueprint layers for the whole application time.

class Blueprints {
  public:
    static Blueprints &GetInstance();

    // Calling SetBlueprintsLocations will reset the stored data and traverse
    // the root locations looking for blueprints
    void SetBlueprintsLocations(const std::vector<std::string> &locations);

    const std::vector<std::string> &GetSubFolders(std::string folder);
    const std::vector<std::pair<std::string, std::string>> &GetItems(std::string folder);

  private:
    std::unordered_map<std::string, std::vector<std::string>> _subFolders;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> _items;
    Blueprints() = default;
    ~Blueprints() = default;
};
