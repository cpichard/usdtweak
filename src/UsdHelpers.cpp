#include "UsdHelpers.h"
#include <iostream>
#include <iomanip>

std::string FindNextAvailableTokenString(std::string prefix) {
    // Find number in the prefix
    size_t end = prefix.size() - 1;
    while (end > 0 && std::isdigit(prefix[end])) {
        end--;
    }
    size_t padding = prefix.size() - 1 - end;
    const std::string number = prefix.substr(end + 1, padding);
    auto value = number.size() ? std::stoi(number) : 0;
    std::ostringstream newName;
    padding = padding == 0 ? 4 : padding; // 4: default padding
    do {
        value += 1;
        newName.seekp(0, std::ios_base::beg); // rewind
        newName << prefix.substr(0, end + 1) << std::setfill('0') << std::setw(padding) << value;
        // Looking for existing token with the same name.
        // There might be a better solution here
    } while (TfToken::Find(newName.str()) != TfToken());
    return newName.str();
}
