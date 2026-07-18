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

vec3 srgbEncode(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec4 a = texture(imageA, uv);
    vec4 b = texture(imageB, uv);
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
