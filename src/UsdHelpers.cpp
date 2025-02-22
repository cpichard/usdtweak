#include "UsdHelpers.h"
#include <iostream>
#include <iomanip>

#include <pxr/usd/sdf/fileFormat.h>


inline void increment(std::string &number) {
    unsigned char ret = 1;
    // if (number == "") number = "0000"; // 4 padding default ? is it useful ?
    for (int i = number.size() - 1; i >= 0; i--) {
        if (ret) {
            number[i] += ret;
        }
        if (number[i] > '9') {
            number[i] = '0';
            ret = 1;
        } else {
            ret = 0;
        }
    }
    if (ret == 1) {
        number = "1" + number;
    }
}

std::string FindNextAvailableTokenString(std::string prefix) {
    // Find number in the prefix
    size_t end = prefix.size() - 1;
    while (end > 0 && std::isdigit(prefix[end])) {
        end--;
    }
    size_t padding = prefix.size() - 1 - end;
    std::string number = prefix.substr(end + 1, padding);
    std::string newName;
    do {
        increment(number);
        newName = prefix.substr(0, end + 1) + number;
    } while (TfToken::Find(newName) != TfToken());
    return newName;
}

const std::vector<std::string> GetUsdValidExtensions() {
    const auto usdExtensions = SdfFileFormat::FindAllFileFormatExtensions();
    std::vector<std::string> validExtensions;
    auto addDot = [](const std::string &str) { return "." + str; };
    std::transform(usdExtensions.cbegin(), usdExtensions.cend(), std::back_inserter(validExtensions), addDot);
    return validExtensions;
}
