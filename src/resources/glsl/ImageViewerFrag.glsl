#version 330 core
// Image viewer display transform and A/B compare: linear in, per-slot
// exposure (stops), compare (A / B / wipe / difference), gamma, sRGB encode
// out. Transparent pixels reveal the background (checker / black / grey) so
// alpha can be judged.
in vec2 uv;
out vec4 outColor;

uniform sampler2D imageA;
uniform sampler2D imageB;
uniform float exposureA; // in stops, 0 = neutral
uniform float exposureB;
uniform float gammaA; // display gamma, 1 = neutral
uniform float gammaB;
uniform int compareMode; // 0 A only, 1 B only, 2 wipe, 3 difference
uniform float wipe;      // wipe position in [0,1], u space
uniform int backgroundMode; // 0 checker, 1 black, 2 grey
uniform int channelMode;    // 0 rgba, 1 r, 2 g, 3 b, 4 alpha, 5 luminance
// B keeps its pixel aspect: normalized to A's width, centered vertically.
// This is B's height relative to the output rect; outside it B is transparent.
uniform float bVerticalScale;

vec3 srgbEncode(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec4 a = texture(imageA, uv);
    vec2 uvB = vec2(uv.x, (uv.y - 0.5) / max(bVerticalScale, 0.0001) + 0.5);
    vec4 b = (uvB.y < 0.0 || uvB.y > 1.0) ? vec4(0.0) : texture(imageB, uvB);
    vec3 la = a.rgb * exp2(exposureA);
    vec3 lb = b.rgb * exp2(exposureB);

    vec3 lin;
    float alpha;
    float gamma;
    if (compareMode == 1) {
        lin = lb;
        alpha = b.a;
        gamma = gammaB;
    } else if (compareMode == 2) {
        bool left = uv.x < wipe;
        lin = left ? la : lb;
        alpha = left ? a.a : b.a;
        gamma = left ? gammaA : gammaB;
    } else if (compareMode == 3) {
        lin = abs(la - lb);
        alpha = 1.0;
        gamma = gammaA;
    } else {
        lin = la;
        alpha = a.a;
        gamma = gammaA;
    }

    // Channel isolation, shown as greyscale (alpha view ignores the background)
    if (channelMode == 1) {
        lin = vec3(lin.r);
        alpha = 1.0;
    } else if (channelMode == 2) {
        lin = vec3(lin.g);
        alpha = 1.0;
    } else if (channelMode == 3) {
        lin = vec3(lin.b);
        alpha = 1.0;
    } else if (channelMode == 4) {
        lin = vec3(clamp(alpha, 0.0, 1.0));
        alpha = 1.0;
    } else if (channelMode == 5) {
        lin = vec3(dot(lin, vec3(0.2126, 0.7152, 0.0722)));
        alpha = 1.0;
    }

    lin = pow(max(lin, vec3(0.0)), vec3(1.0 / max(gamma, 0.01)));
    vec3 color = srgbEncode(clamp(lin, 0.0, 1.0));

    vec3 bg;
    if (backgroundMode == 1) {
        bg = vec3(0.0);
    } else if (backgroundMode == 2) {
        bg = vec3(0.5);
    } else {
        // Checker in image pixel space, 8x8 squares
        ivec2 square = ivec2(gl_FragCoord.xy) / 8;
        bg = (((square.x + square.y) & 1) == 0) ? vec3(0.28) : vec3(0.42);
    }
    // Straight-alpha blend; a premultiplied/straight toggle is planned for v2
    alpha = clamp(alpha, 0.0, 1.0);
    outColor = vec4(mix(bg, color, alpha), 1.0);
}
