#pragma once

/// Draw the search panel ImGui window contents.
/// Call this inside an ImGui::Begin / ImGui::End block each frame.
void DrawSearchWidget();

/// Draw the UTQL help window, when the user has opened it from the search panel.
/// It is a window of its own, so call this outside the search panel's
/// ImGui::Begin / ImGui::End block.
void DrawUtqlHelp();
