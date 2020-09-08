#include <iostream>
#include "Gui.h"
#include "Timeline.h"

// Draws slider in lieu of the timeline
void DrawTimeline(UsdStage &stage, UsdTimeCode &currentTimeCode) {

    int startTime = static_cast<int>(stage.GetStartTimeCode());
    int endTime = static_cast<int>(stage.GetEndTimeCode());
    int currentTime = static_cast<int>(currentTimeCode.GetValue());

    ImGui::InputInt("Start", &startTime);
    ImGui::InputInt("End", &endTime);
    ImGui::InputInt("Frame", &currentTime);
    ImGui::SliderInt("", &currentTime, startTime, endTime);
    if (currentTimeCode.GetValue() != static_cast<double>(currentTime)) {
        currentTimeCode = static_cast<UsdTimeCode>(currentTime);
    }
    if (stage.GetStartTimeCode() != static_cast<double>(startTime)) {
        stage.SetStartTimeCode(static_cast<double>(startTime));
    }
    if (stage.GetEndTimeCode() != static_cast<double>(endTime)) {
        stage.SetEndTimeCode(static_cast<double>(endTime));
    }
}
