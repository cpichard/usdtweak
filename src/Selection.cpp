#include <functional>
#include "Selection.h"

// TODO create a class for the selection to avoid statics.
// At the moment there is only one selection object in editor but when we'll
// use multiple selection this won't work at all

static SelectionHash selectionHash = 0;
static bool mustRecomputeHash = true;

/// Note the following bit of code might have terrible performances when selecting thousands of paths
/// This has to be tested at some point
namespace std {
template <> struct hash<SdfPathVector> {
    std::size_t operator()(SdfPathVector const &paths) const noexcept {
        std::size_t hashValue = 0;
        for (const auto p : paths) {
            const auto pathHash = SdfPath::Hash{}(p);
            hashValue = pathHash ^ (hashValue << 1);
        }
        return hashValue;
    }
};
} // namespace std

static std::size_t GetSelectionHash(const Selection &selection) {
    if (selection && mustRecomputeHash) {
        auto paths = selection->GetAllSelectedPrimPaths();
        selectionHash = std::hash<SdfPathVector>{}(paths);
    }
    return selectionHash;
}

/// Clear selection, editor implementation
void ClearSelection(Selection &selection) {
    selection.reset(new HdSelection());
    mustRecomputeHash = true;
}

void AddSelection(Selection &selection, const SdfPath &path) {
    if (!selection) {
        selection.reset(new HdSelection());
    }
    selection->AddRprim(HdSelection::HighlightModeSelect, path);
    mustRecomputeHash = true;
}

void SetSelected(Selection &selection, const SdfPath &path) {
    selection.reset(new HdSelection());
    selection->AddRprim(HdSelection::HighlightModeSelect, path);
    mustRecomputeHash = true;
}

bool IsSelected(const Selection &selection, const SdfPath &path) {
    if (selection) {
        if (selection->GetPrimSelectionState(HdSelection::HighlightModeSelect, path)) {
            return true;
        }
    }
    return false;
}

bool IsSelectionEmpty(const Selection &selection) { return !selection || selection->IsEmpty(); }

bool UpdateSelectionHash(const Selection &selection, SelectionHash &lastSelectionHash) {
    if (GetSelectionHash(selection) != lastSelectionHash) {
        lastSelectionHash = GetSelectionHash(selection);
        return true;
    }
    return false;
}



SdfPath GetSelectedPath(const Selection &selection) {
    if (selection) {
        const auto &paths = selection->GetSelectedPrimPaths(HdSelection::HighlightModeSelect);
        if (!paths.empty()) {
            return paths[0];
        }
    }
    return {};
}

std::vector<SdfPath> GetSelectedPaths(const Selection &selection) {
    if (selection) {
        return selection->GetAllSelectedPrimPaths();
    }
    return {};
}
