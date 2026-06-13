#pragma once

#include "Gui.h"
#include "UsdaLexer.h"

/// Syntax colors for the USDA text editor (One Dark inspired palette).
inline ImU32 UsdaTokenColor(UsdaTokenType type) {
    switch (type) {
    case UsdaTokenType::Keyword:
        return IM_COL32(198, 120, 221, 255); // purple
    case UsdaTokenType::Literal:
        return IM_COL32(209, 154, 102, 255); // orange
    case UsdaTokenType::Identifier:
        return IM_COL32(171, 178, 191, 255); // light gray
    case UsdaTokenType::Number:
        return IM_COL32(209, 154, 102, 255); // orange
    case UsdaTokenType::String:
        return IM_COL32(152, 195, 121, 255); // green
    case UsdaTokenType::AssetRef:
        return IM_COL32(86, 182, 194, 255); // cyan
    case UsdaTokenType::PathRef:
        return IM_COL32(97, 175, 239, 255); // blue
    case UsdaTokenType::Punctuation:
        return IM_COL32(130, 137, 151, 255); // gray
    case UsdaTokenType::Comment:
        return IM_COL32(92, 99, 112, 255); // dark gray
    case UsdaTokenType::MagicHeader:
        return IM_COL32(92, 99, 112, 255); // dark gray
    case UsdaTokenType::FoldedValue:
        return IM_COL32(229, 192, 123, 255); // yellow
    case UsdaTokenType::Error:
        return IM_COL32(224, 108, 117, 255); // red
    }
    return IM_COL32(171, 178, 191, 255);
}

constexpr ImU32 TextEditorLineNumberColor = IM_COL32(99, 109, 131, 255);
constexpr ImU32 TextEditorSelectionColor = IM_COL32(62, 68, 81, 255);
constexpr ImU32 TextEditorCursorLineColor = IM_COL32(255, 255, 255, 8);
