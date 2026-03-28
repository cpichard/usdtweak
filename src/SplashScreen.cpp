#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#include "3rdparty/imgui/stb_image.h"

#include "SplashScreen.h"
#include "SplashScreenImage.h"
#include <pxr/imaging/garch/glApi.h>
#include <algorithm>

const SplashTexture &GetSplashTexture() {
    static SplashTexture tex;
    if (tex.id == 0) {
        int w = 0, h = 0, channels = 0;
        unsigned char *data = stbi_load_from_memory(splashScreenData, static_cast<int>(splashScreenDataSize),
                                                    &w, &h, &channels, 4);
        if (data) {
            GLuint glTex = 0;
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            stbi_image_free(data);
            tex.id = static_cast<unsigned int>(glTex);
            tex.width = w;
            tex.height = h;
        }
    }
    return tex;
}


float DrawAboutHeader() {
    const SplashTexture &tex = GetSplashTexture();
    if (tex.id == 0)
        return ImGui::GetCursorScreenPos().y;
    const float availW = ImGui::GetContentRegionAvail().x;
    const float scale = availW / static_cast<float>(tex.width);
    const ImVec2 imageSize(availW, tex.height * scale);

    // Draw image as background via draw list (does not advance the cursor).
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + imageSize.x, p0.y + imageSize.y);
    ImGui::GetWindowDrawList()->AddImage(
        static_cast<ImTextureID>(static_cast<uintptr_t>(tex.id)), p0, p1);

    // Reserve space so the window is sized to the image,
    // then reset cursor to p0 so subsequent items render on top.
    ImGui::Dummy(imageSize);
    ImGui::SetCursorScreenPos(p0);
    return p1.y; // screen-space bottom of the image
}
