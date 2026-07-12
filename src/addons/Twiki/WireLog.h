#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace UsdAgent {

// Optional debugging sink that appends the raw traffic of the agent to a file:
// every HTTP request body sent to the provider, every response body received,
// and every tool call + tool result the orchestrator processes. Off by default;
// enabled from the Settings tab.
//
// Threading: the worker thread writes (backend Send + orchestrator loop) while
// the UI thread may toggle enable/path — every access is mutex-guarded. The sink
// is a panel member; the backend and orchestrator hold a raw pointer to it, so
// the panel must outlive any in-flight turn (its destructor already waits on the
// pending future).
class WireLog {
public:
    // Turn logging on/off. When enabling, opens `path` for appending and writes
    // a session banner. Returns false + *err (and stays disabled) if the file
    // can't be opened. Disabling closes the file.
    bool SetEnabled(bool on, const std::string& path, std::string* err = nullptr);
    bool        IsEnabled() const;
    std::string Path()      const;

    // A default log path under the OS temp dir. Handy seed for the UI.
    static std::string DefaultPath();

    // Section writers — all no-ops when disabled.
    void Turn      (const std::string& userMessage);   // turn boundary marker
    void Request   (const std::string& provider, const std::string& model,
                    const std::string& rawBody);
    void Response  (int status, const std::string& rawBody);
    void ToolCall  (const std::string& name, const std::string& argsJson);
    void ToolResult(const std::string& name, const std::string& result);

private:
    void _Write(const std::string& heading, const std::string& body);  // caller holds _mu

    mutable std::mutex _mu;
    bool          _enabled = false;
    std::string   _path;
    std::ofstream _out;
};

} // namespace UsdAgent
