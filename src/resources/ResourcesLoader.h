#pragma once
#include "EditorSettings.h"

 class ResourcesLoader {
  public:
    ResourcesLoader();
    ~ResourcesLoader();

    // Return the settings that are going to be stored on disk
    static EditorSettings & GetEditorSettings();

 private:

     static EditorSettings _editorSettings;
};
