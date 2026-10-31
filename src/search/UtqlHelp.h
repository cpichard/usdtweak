#pragma once

#include <functional>
#include <string>

/// Called when the user picks a query from the help page: `run` is true when the
/// query should be executed straight away, false when it should only be loaded
/// into the query editor.
using UtqlHelpUseQueryFn = std::function<void(const std::string &query, bool run)>;

/// Draw the UTQL help window (table of contents + the embedded help page).
/// Does nothing when *open is false. Call outside of any other window's
/// Begin/End block.
void DrawUtqlHelpWindow(bool *open, const UtqlHelpUseQueryFn &useQuery);
