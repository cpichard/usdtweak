#include <iostream>
#include "Timeline.h"
#include "Commands.h"
#include "Gui.h"

// The easiest version of a timeline: a slider
void DrawTimeline(UsdStageRefPtr stage, UsdTimeCode &currentTimeCode) {
    if (!stage) return;
    int startTime = static_cast<int>(stage->GetStartTimeCode());
    ImGui::InputInt("Start", &startTime);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (stage->GetStartTimeCode() != static_cast<double>(startTime)) {
            ExecuteAfterDraw(&UsdStage::SetStartTimeCode, stage, static_cast<double>(startTime));
        }
    }

    int endTime = static_cast<int>(stage->GetEndTimeCode());
    ImGui::InputInt("End", &endTime);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (stage->GetEndTimeCode() != static_cast<double>(endTime)) {
            ExecuteAfterDraw(&UsdStage::SetEndTimeCode, stage, static_cast<double>(endTime));
        }
    }

    int currentTime = static_cast<int>(currentTimeCode.GetValue());
    ImGui::InputInt("Frame", &currentTime);
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        if (currentTimeCode.GetValue() != static_cast<double>(currentTime)) {
            currentTimeCode = static_cast<UsdTimeCode>(currentTime);
        }
    }

    int currentTimeSlider = static_cast<int>(currentTimeCode.GetValue());
    ImGui::SliderInt("##SliderFrame", &currentTimeSlider, startTime, endTime);
    if (currentTimeCode.GetValue() != static_cast<double>(currentTimeSlider)) {
        currentTimeCode = static_cast<UsdTimeCode>(currentTimeSlider);
    }

}
