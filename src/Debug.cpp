#include "Gui.h"
#include "Debug.h"
#include "pxr/base/trace/reporter.h"
#include "pxr/base/trace/trace.h"
#include <pxr/base/tf/debug.h>

PXR_NAMESPACE_USING_DIRECTIVE

static void DrawTraceReporter() {

    static std::string reportStr;

    if (ImGui::Button("Start Tracing")) {
        TraceCollector::GetInstance().SetEnabled(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop Tracing")) {
        TraceCollector::GetInstance().SetEnabled(false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset counters")) {
        TraceReporter::GetGlobalReporter()->ClearTree();
    }
    ImGui::SameLine();

    if (ImGui::Button("Update tree")) {
        TraceReporter::GetGlobalReporter()->UpdateTraceTrees();
    }
    if (TraceCollector::IsEnabled()) {
        std::ostringstream report;
        TraceReporter::GetGlobalReporter()->Report(report);
        reportStr = report.str();
    }
    ImGuiIO &io = ImGui::GetIO();
    ImGui::PushFont(io.Fonts->Fonts[1]);
    const ImVec2 size(-FLT_MIN, -10);
    ImGui::InputTextMultiline("##TraceReport", &reportStr, size);
    ImGui::PopFont();
}

static void DrawDebugCodes() {
    // TfDebug::IsCompileTimeEnabled()
    ImVec2 listBoxSize(-FLT_MIN, -10);
    if (ImGui::BeginListBox("##DebugCodes", listBoxSize)) {
        for (auto &code : TfDebug::GetDebugSymbolNames()) {
            bool isEnabled = TfDebug::IsDebugSymbolNameEnabled(code);
            if (ImGui::Checkbox(code.c_str(), &isEnabled)) {
                TfDebug::SetDebugSymbolsByName(code, true);
            }
        }
        ImGui::EndListBox();
    }
}

// Draw a preference like panel
void DrawDebugUI() {
    static const char *const panels[] = {"Timings", "Debug codes", "Trace reporter"};
    static int current_item = 0;
    ImGui::PushItemWidth(100);
    ImGui::ListBox("##DebugPanels", &current_item, panels, 3);
    ImGui::SameLine();
    if (current_item == 0) {
        ImGui::BeginChild("##Timing");
        ImGui::Text("ImGui: %.3f ms/frame  (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);
        ImGui::EndChild();
    } else if (current_item == 1) {
        ImGui::BeginChild("##DebugCodes");
        DrawDebugCodes();
        ImGui::EndChild();
    } else if (current_item == 2) {
        ImGui::BeginChild("##TraceReporter");
        DrawTraceReporter();
        ImGui::EndChild();
    }
}
