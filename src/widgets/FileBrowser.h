#include <string>

/// Draw the file browser ui
/// this can be used anywhere, except that there is only one instance
/// of the FileBrowser
void DrawFileBrowser();

/// Returns the current file browser path
std::string GetFileBrowserFilePath();

/// Returns true if the current file browser path exists
bool FilePathExists();