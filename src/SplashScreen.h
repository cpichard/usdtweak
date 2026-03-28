#pragma once
#include "3rdparty/imgui/imgui.h"

// Texture loaded lazily from embedded JPEG data (800x565).
// id==0 means not yet loaded or failed.
struct SplashTexture {
    unsigned int id = 0; // GLuint, avoid pulling in OpenGL header here
    int width = 0;
    int height = 0;
};

// Load (once) and return the splash screen texture. Safe to call every frame.
const SplashTexture &GetSplashTexture();

// Draw the splash image as a full-width background. Call at the top of the About dialog.
// Returns the screen-space bottom Y of the image so the caller can position items below it.
float DrawAboutHeader();
