#include "Blueprints.h"
#include <cctype>
#include <iostream>
#include <set>
#include <stack>
//#include <pxr/base/arch/fileSystem.h>
#include <pxr/usd/sdf/fileFormat.h>

#include "FileSystem.h"

PXR_NAMESPACE_USING_DIRECTIVE

// Blueprint names are displayed with an upper case first letter
static std::string CapitalizedStem(const fs::path &path) {
    std::string stem = ToGenericUtf8(path.stem());
    if (!stem.empty()) {
        stem[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(stem[0])));
    }
    return stem;
}

void Blueprints::SetBlueprintsLocations(const std::vector<std::string> &locations) {
    std::cout << "Reading blueprints" << std::endl;
    const std::set<std::string> allUsdExt = SdfFileFormat::FindAllFileFormatExtensions();
    // Directories left to read, with the blueprint folder name they map to
    std::stack<std::pair<fs::path, std::string>> directories;
    for (const auto &loc : locations) {
        directories.emplace(FromUtf8(loc), "");
    }
    // Canonical paths of the directories already read, a symlink to a parent directory would loop forever
    std::set<fs::path> visited;
    const auto CanonicalPath = [](const fs::path &path) {
        std::error_code ec;
        const fs::path canonicalPath = fs::canonical(path, ec);
        return ec ? path : canonicalPath;
    };
    _subFolders.clear();
    _items.clear();
    // This runs at startup in the Editor constructor: an exception here would prevent usdtweak from starting,
    // so only the non throwing filesystem functions are used.
    // TODO: we should measure the time it takes to read all the blueprints as this can affect the application startup time
    // Also this code could be ran in another thread as it is not essential to have the
    // blueprint informations at startup time
    // -> We might just want to read a json file containing a list of file/
    // if there is a json file; otherwise read the dir ??
    while (!directories.empty()) {
        const fs::path directory = directories.top().first;
        const std::string folder = directories.top().second;
        directories.pop();
        std::error_code ec;
        if (!fs::is_directory(directory, ec)) {
            std::cerr << "unable to find blueprint directory " << ToUtf8(directory) << std::endl;
            continue;
        }
        if (!visited.insert(CanonicalPath(directory)).second) {
            continue;
        }
        fs::directory_iterator it(directory, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const fs::directory_entry &entry = *it;
            std::error_code entryError;
            if (entry.is_directory(entryError)) {
                if (visited.count(CanonicalPath(entry.path()))) {
                    continue; // link to a directory already read, typically a parent
                }
                const std::string folderStem = CapitalizedStem(entry.path());
                if (!folderStem.empty()) {
                    const std::string subFolderName = folder + "/" + folderStem;
                    directories.emplace(entry.path(), subFolderName);
                    _subFolders[folder].push_back(subFolderName);
                }
            } else if (entry.is_regular_file(entryError)) {
                const std::string layerPath = ToGenericUtf8(entry.path());
                const auto ext = SdfFileFormat::GetFileExtension(layerPath);
                if (allUsdExt.find(ext) != allUsdExt.end()) {
                    const std::string itemName = CapitalizedStem(entry.path());
                    if (!itemName.empty()) {
                        _items[folder].push_back(std::make_pair(itemName, layerPath));
                    }
                }
            }
        }
        if (ec) {
            std::cerr << "unable to read blueprint directory " << ToUtf8(directory) << ": " << ec.message() << std::endl;
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
