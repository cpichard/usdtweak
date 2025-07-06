#include "HydraNoticeLogger.h"

#include "Constants.h"
#include "Gui.h"
#include "ImGuiHelpers.h"
#include "VtValueEditor.h"

#include <pxr/pxr.h> // for PXR_VERSION
#include <stack>

#if PXR_VERSION < 2302
void DrawHydraNoticeLogger() { ImGui::Text("Hydra notice logger is not supported in this version of USD"); }
#else

#include <pxr/imaging/hd/filteringSceneIndex.h>
#include <pxr/imaging/hd/retainedDataSource.h>

#include "HydraWidgets.h"

PXR_NAMESPACE_USING_DIRECTIVE
#define HydraNoticeLoggerSeed 2323734
#define IdOf ToImGuiID<HydraNoticeLoggerSeed, size_t>

// TODO add a copy logs to clipboard
// TODO allow selection of the primPath in the HydraBrowser

// Hashing of HdDataSourceLocatorSet
PXR_NAMESPACE_OPEN_SCOPE
template <typename HashState> inline void TfHashAppend(HashState &h, const HdDataSourceLocatorSet &locatorSet) {
    h.AppendRange(locatorSet.begin(), locatorSet.end());
}
PXR_NAMESPACE_CLOSE_SCOPE

using LocatorsID = uint64_t;

class HydraNoticeLogger {

    struct LogEntry {

        LogEntry(const LogEntry &logEntry)
            : logType(logEntry.logType), primPath(logEntry.primPath), repetitions(logEntry.repetitions) {
            if (IsAdded()) {
                primType = logEntry.primType;
            } else if (IsDirtied()) {
                locatorsID = logEntry.locatorsID;
            } else if (IsRenamed()) {
                newPrimPath = logEntry.newPrimPath;
            }
        }

        ~LogEntry() {
            if (IsRenamed()) {
                newPrimPath.~SdfPath();
            } else if (IsAdded()) {
                primType.~TfToken();
            }
        }
        // Added constructor
        LogEntry(const SdfPath &primPath, const TfToken &primType_) : primPath(primPath), logType(0), repetitions(1) {
            primType = primType_;
        }

        // Removed constructor
        LogEntry(const SdfPath &primPath) : primPath(primPath), logType(1), repetitions(1) {}

        // Dirtied constructor
        LogEntry(const SdfPath &primPath, const HdDataSourceLocatorSet &locators)
            : primPath(primPath), logType(2), repetitions(1) {
            locatorsID = static_cast<uint32_t>(TfHash{}(locators));
            const auto &locatorsIt = locatorsMap.find(locatorsID);
            if (locatorsIt != locatorsMap.end()) {
                std::stringstream k;
                k << locators;
                locatorsMap[locatorsID] = k.str();
            }
        }

        // Rename constructor
        LogEntry(const SdfPath &oldPrimPath, const SdfPath &newPrimPath_) : primPath(primPath), logType(3), repetitions(1) {
            newPrimPath = newPrimPath_;
        }

        bool operator==(const LogEntry &rhs) const {
            return rhs.logType == this->logType && rhs.primPath == this->primPath &&
                   ((IsAdded() && rhs.primType == this->primType) || (IsRemoved()) ||
                    (IsDirtied() && rhs.locatorsID == this->locatorsID) || (IsRenamed() && rhs.newPrimPath == this->newPrimPath));
        }

        // TODO use enum for logType or alternatively pack logType with repetitions
        inline bool IsAdded() const { return logType == 0; }
        inline bool IsRemoved() const { return logType == 1; }
        inline bool IsDirtied() const { return logType == 2; }
        inline bool IsRenamed() const { return logType == 3; }

        SdfPath primPath;
        // TODO check std::variant size before replacing union with variant
        union {
            TfToken primType;
            SdfPath newPrimPath;
            LocatorsID locatorsID; // ID in the locatorsMap
        };
        uint32_t repetitions;
        uint8_t logType; // TODO we could pack repetitions and logType into one variable to reduce the structure size

        // We assume that the set of HdDataSourceLocatorSet is small, ie that the same sets of locators
        // are used very often. We store the string representation in a map
        static std::unordered_map<LocatorsID, std::string> locatorsMap;
    };

    using LogEntryVector = std::vector<LogEntry>;

    class SceneIndexObserver : public HdSceneIndexObserver {
      public:
        void PrimsAdded(const HdSceneIndexBase &sender, const AddedPrimEntries &entries) override {
            for (const auto &entry : entries) {
                LogEntry logEntry(entry.primPath, entry.primType);
                PushInLogs(logEntry);
            }
        }

        void PrimsRemoved(const HdSceneIndexBase &sender, const RemovedPrimEntries &entries) override {
            for (const auto &entry : entries) {
                LogEntry logEntry(entry.primPath);
                PushInLogs(logEntry);
            }
        }

        void PrimsDirtied(const HdSceneIndexBase &sender, const DirtiedPrimEntries &entries) override {
            for (const auto &entry : entries) {
                LogEntry logEntry(entry.primPath, entry.dirtyLocators);
                PushInLogs(logEntry);
            }
        }

        void PrimsRenamed(const HdSceneIndexBase &sender, const RenamedPrimEntries &entries) override {
            for (const auto &entry : entries) {
                LogEntry logEntry(entry.oldPrimPath, entry.newPrimPath);
                PushInLogs(logEntry);
            }
        }

        void DrawLogEntries() {
            int idx = 0;
            constexpr ImGuiTableFlags tableFlags = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
            if (ImGui::BeginTable("##HydraNoticeLogger", 1, tableFlags)) {
                ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                ImGui::TableSetColumnIndex(0);
                // TODO use a Clipper and colums
                // We want to see the latest event on top to avoid scrolling
                for (auto entryIt = logs.rbegin(); entryIt != logs.rend(); ++entryIt) {
                    const auto &entry = *entryIt;
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    if (entry.IsDirtied()) {
                        ImGui::Text("%05d Dirtied %s %s", entry.repetitions, entry.primPath.GetString().c_str(),
                                    LogEntry::locatorsMap[entry.locatorsID].c_str());
                    } else if (entry.IsAdded()) {
                        ImGui::Text("%05d Added %s %s", entry.repetitions, entry.primPath.GetString().c_str(),
                                    entry.primType.GetString().c_str());
                    } else if (entry.IsRemoved()) {
                        ImGui::Text("%05d Removed %s", entry.repetitions, entry.primPath.GetString().c_str());
                    } else if (entry.IsRenamed()) {
                        ImGui::Text("%05d Renamed %s %s", entry.repetitions, entry.primPath.GetString().c_str(),
                                    entry.newPrimPath.GetString().c_str());
                    }
                }
                ImGui::EndTable();
            }
        }

        void Clear() { logs.clear(); }

        LogEntryVector logs;

      private:
        inline void PushInLogs(LogEntry &logEntry) {
            if (!logs.empty() && logs.back() == logEntry) {
                logs.back().repetitions++;
            } else {
                logs.push_back(logEntry);
            }
        }
    };

  public:
    void StartLogging(HdSceneIndexBaseRefPtr sceneIndex) {
        if (!sceneIndex)
            return;
        if (!_isLogging) {
            _isLogging = true;
            // Find the scene index
            sceneIndex->AddObserver(HdSceneIndexObserverPtr(&_observer));
        }
    }
    void StopLogging(HdSceneIndexBaseRefPtr sceneIndex) {
        if (_isLogging) {
            _isLogging = false;
            sceneIndex->RemoveObserver(HdSceneIndexObserverPtr(&_observer));
        }
    }

    void DrawLogs() { _observer.DrawLogEntries(); }

    void Clear() { _observer.Clear(); }

    bool _isLogging = false;
    SceneIndexObserver _observer;
};

std::unordered_map<LocatorsID, std::string> HydraNoticeLogger::LogEntry::locatorsMap;

void DrawHydraNoticeLogger() {
    static std::string selectedSceneIndexName;
    static std::unordered_map<std::string, std::string> selectedInputNamePerSI; // per scene index
    static std::unordered_map<std::string, HdSceneIndexBasePtr> selectedFilterPerSI;
    static HydraNoticeLogger logger;

    std::string &selectedInputName = selectedInputNamePerSI[selectedSceneIndexName];
    HdSceneIndexBasePtr &selectedFilter = selectedFilterPerSI[selectedSceneIndexName];
    if (selectedFilter) {
    }
    if (logger._isLogging) {
        ImGui::Text("%s", selectedSceneIndexName.c_str());
        ImGui::Text("Logging %s", selectedFilter->GetDisplayName().c_str());
        if (ImGui::Button("Stop logging")) {
            logger.StopLogging(selectedFilter);
        }
    } else {
        DrawSceneIndexSelector(selectedSceneIndexName, selectedInputName);
        DrawSceneIndexFilterSelector(selectedSceneIndexName, selectedFilter);
        if (selectedFilter) {
            if (ImGui::Button("Start logging")) {
                logger.StartLogging(selectedFilter);
            }
        }
    }
    if (selectedFilter) {
        ImGui::SameLine();
        if (ImGui::Button("Clear")) {
            logger.Clear();
        }
        logger.DrawLogs();
    }
}
#endif // PXR_VERSION < 2302
