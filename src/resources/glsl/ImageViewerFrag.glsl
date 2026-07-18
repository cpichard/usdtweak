#version 330 core
// Image viewer display transform: linear in, exposure (stops) and gamma
// applied in linear space, sRGB encode out. Transparent pixels reveal the
// background (checker / black / grey) so alpha can be judged.
in vec2 uv;
out vec4 outColor;

uniform sampler2D image;
uniform float exposure; // in stops, 0 = neutral
uniform float gamma;    // display gamma, 1 = neutral
uniform int backgroundMode; // 0 checker, 1 black, 2 grey

vec3 srgbEncode(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec4 texel = texture(image, uv);
    vec3 color = texel.rgb * exp2(exposure);
    color = pow(max(color, vec3(0.0)), vec3(1.0 / max(gamma, 0.01)));
    color = srgbEncode(clamp(color, 0.0, 1.0));

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
    float alpha = clamp(texel.a, 0.0, 1.0);
    outColor = vec4(mix(bg, color, alpha), 1.0);
}
