#include "WireLog.h"

#include <chrono>
#include <ctime>
#include <filesystem>

namespace UsdAgent {

namespace {

std::string _Timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

} // namespace

std::string WireLog::DefaultPath() {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = ".";
    return (dir / "twiki-wirelog.txt").string();
}

bool WireLog::SetEnabled(bool on, const std::string& path, std::string* err) {
    std::lock_guard<std::mutex> g(_mu);
    if (!on) {
        if (_out.is_open()) _out.close();
        _enabled = false;
        return true;
    }

    // Reopen if the path changed or the stream is not open.
    if (!_out.is_open() || path != _path) {
        if (_out.is_open()) _out.close();
        _out.open(path, std::ios::out | std::ios::app);
        if (!_out) {
            _enabled = false;
            if (err) *err = "could not open log file: " + path;
            return false;
        }
        _path = path;
    }
    _enabled = true;
    _out << "\n========== WIRE LOG SESSION START " << _Timestamp()
         << " ==========\n" << std::flush;
    return true;
}

bool WireLog::IsEnabled() const {
    std::lock_guard<std::mutex> g(_mu);
    return _enabled;
}

std::string WireLog::Path() const {
    std::lock_guard<std::mutex> g(_mu);
    return _path;
}

void WireLog::_Write(const std::string& heading, const std::string& body) {
    // Caller holds _mu.
    if (!_enabled || !_out.is_open()) return;
    _out << "\n----- [" << _Timestamp() << "] " << heading << " -----\n"
         << body << '\n' << std::flush;
}

void WireLog::Turn(const std::string& userMessage) {
    std::lock_guard<std::mutex> g(_mu);
    _Write("TURN user=" + userMessage, "");
}

void WireLog::Request(const std::string& provider, const std::string& model,
                      const std::string& rawBody) {
    std::lock_guard<std::mutex> g(_mu);
    _Write("REQUEST " + provider + " / " + model, rawBody);
}

void WireLog::Response(int status, const std::string& rawBody) {
    std::lock_guard<std::mutex> g(_mu);
    _Write("RESPONSE http=" + std::to_string(status), rawBody);
}

void WireLog::ToolCall(const std::string& name, const std::string& argsJson) {
    std::lock_guard<std::mutex> g(_mu);
    _Write("TOOL_CALL " + name, argsJson);
}

void WireLog::ToolResult(const std::string& name, const std::string& result) {
    std::lock_guard<std::mutex> g(_mu);
    _Write("TOOL_RESULT " + name, result);
}

} // namespace UsdAgent
