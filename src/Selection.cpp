#include <functional>
#include "Selection.h"

/// One possible implementation with HdSelection

/// Clear selection, editor implementation
void ClearSelection(Selection &selection) {
    selection.reset(new HdSelection());
}

void AddSelection(Selection &selection, const SdfPath &path) {
    if (!selection) {
        selection.reset(new HdSelection());
    }
    selection->AddRprim(HdSelection::HighlightModeSelect, path);
}

void SetSelected(Selection &selection, const SdfPath &path) {
    selection.reset(new HdSelection());
    selection->AddRprim(HdSelection::HighlightModeSelect, path);
}

bool IsSelected(const Selection &selection, const SdfPath &path) {
    if (selection) {
        if (selection->GetPrimSelectionState(HdSelection::HighlightModeSelect, path)) {
            return true;
        }
    }
    return false;
}


/// Note the following bit of code might have terrible performances when selecting thousands of paths
/// This has to be tested at some point
namespace std
{
    template<> struct hash<SdfPathVector>
    {
        std::size_t operator()(SdfPathVector const& paths) const noexcept
        {
            std::size_t hashValue = 0;
            for (const auto p : paths) {
                const auto pathHash = SdfPath::Hash{}(p);
                hashValue = pathHash ^ (hashValue << 1);
            }
            return hashValue;
        }
    };
}

std::size_t GetSelectionHash(Selection &selection) {
    if (selection) {
        auto paths = selection->GetAllSelectedPrimPaths();
        return std::hash<SdfPathVector>{}(paths);
    }
    return 0;
}

SdfPath GetSelectedPath(Selection & selection) {

    if (selection) {
        const auto & paths = selection->GetSelectedPrimPaths(HdSelection::HighlightModeSelect);
        if (!paths.empty()) {
            return paths[0];
        }
    }
    return {};
}

std::vector<SdfPath> GetSelectedPaths(Selection & selection) {
    if (selection) {
        return selection->GetAllSelectedPrimPaths();
    }

    return {};
}
