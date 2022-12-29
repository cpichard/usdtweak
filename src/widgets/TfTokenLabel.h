#pragma once


// Returns a nicer label for the predefined tokens
inline
const char * GetTokenLabel(const TfToken &token) {
    if (token == SdfFieldKeys->CustomData) {
        return "Custom Data";
    } else if (token == SdfFieldKeys->AssetInfo) {
        return "Asset Info";
    } // add others as needed
    return token.GetText();
}
