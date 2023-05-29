#include "Blueprints.h"
#include <iostream>
#include <stack>
//#include <pxr/base/arch/fileSystem.h>
#include <pxr/usd/sdf/fileFormat.h>

#if defined(__cplusplus) && __cplusplus >= 201703L && defined(__has_include) && __has_include(<filesystem>)
#include <filesystem>
namespace fs = std::filesystem;
#else
#define GHC_WITH_EXCEPTIONS 0
#include <ghc/filesystem.hpp>
namespace fs = ghc::filesystem;
#endif

PXR_NAMESPACE_USING_DIRECTIVE

void Blueprints::SetBlueprintsLocations(const std::vector<std::string> &locations) {
    std::cout << "Reading blueprints" << std::endl;
    const std::set<std::string> allUsdExt = SdfFileFormat::FindAllFileFormatExtensions();
    std::stack<std::string> folders;
    std::stack<std::string> paths;
    for (const auto &loc : locations) {
        paths.push(loc);
        folders.push("");
    }
    _subFolders.clear();
    _items.clear();
    // TODO: we should measure the time it takes to read all the blueprints as this can affect the application startup time
    // Also this code could be ran in another thread as it is not essential to have the
    // blueprint informations at startup time
    while (!paths.empty()) {
        const auto path = fs::directory_iterator(paths.top());
        paths.pop();
        const auto folder = folders.top();
        folders.pop();
        for (const auto &entry : fs::directory_iterator(path)) {
            if (fs::is_directory(entry)) {
                paths.push(entry.path().generic_string());
                std::string folderStem = std::prev(entry.path().end())->generic_string();
                if (!folderStem.empty()) {
                    folderStem[0] = std::toupper(folderStem[0]);
                    std::string subFolderName = folder + "/" + folderStem;
                    folders.push(subFolderName);
                    _subFolders[folder].push_back(subFolderName);
                }
            } else if (fs::is_regular_file(entry.path())) {
                std::string layerPath = entry.path().generic_string();
                const auto ext = SdfFileFormat::GetFileExtension(layerPath);
                if (allUsdExt.find(ext) != allUsdExt.end()) {
                    std::string itemName = entry.path().stem().generic_string();
                    if (!itemName.empty()) {
                        itemName[0] = std::toupper(itemName[0]);
                        _items[folder].push_back(std::make_pair(itemName, layerPath));
                    }
                }
            }
        }
    }
    std::cout << "Blueprints ready" << std::endl;
}

Blueprints &Blueprints::GetInstance() {
    static Blueprints instance;
    return instance;
}
const std::vector<std::string> &Blueprints::GetSubFolders(std::string folder) { return _subFolders[folder]; }
const std::vector<std::pair<std::string, std::string>> &Blueprints::GetItems(std::string folder) { return _items[folder]; }
