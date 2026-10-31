// Experimental replacement of the outline shader shipped with hdx (outline.glslfx).
// Compiled by Hgi: no version line, and uvOut, radius, texelSize, hd_FragColor and
// HgiGet_colorIn are declared by the shader description in GLEngineOutlineTask.
//
// texelSize is the size of an output pixel in uv, whatever the mask resolution. Storm
// gives a supersampled mask, 2 mask texels per output pixel on each axis: the distance
// to the selection is sub-pixel and both edges of the line are smooth, and with the
// linear sampler of the task one read at a pixel offset tells if any of the 4 texels
// around it is selected. The other renderers give a mask with one texel per pixel,
// for which the filtered reads below are a smoothed version of that mask.
// Only the cells with a hit are read texel by texel, so empty areas cost
// about one read per output pixel of the search area.
//
// The mask is binary, white where selected, and the filtered reads make it necessary
// to threshold it: rounding in the texture filter leaks a tiny value from a selected
// texel into its neighbors, which must not count as selected. The line color comes
// from outlineColor, never from the mask.
//
// The output is pre-multiplied with the remaining scene weight in alpha, so the
// line covers the scene instead of adding to it. The line also writes the near depth,
// so the grid and the guides drawn after the scene stay behind it. Every other pixel
// is discarded to keep the scene depth.

const float kTexelSelected = 0.5;  // a read at a texel center
const float kAnyTexelSelected = 0.1; // a read between 4 texels, one selected gives 0.25
// Close to the near plane. Not 0: the transfer to the viewport turned exactly 0 into 1, the
// empty background depth (measured on macOS with Metal), which the grid ignored.
const float kLineDepth = 0.001;
const float kTexelOffset = 0.25;   // from a pixel center to its 4 texel centers, in pixels
const float kCellReach = 0.36;     // from a pixel center to its furthest texel center, in pixels

// offset is in output pixels
vec4 readMask(vec2 offset) {
    return HgiGet_colorIn(uvOut + offset * texelSize);
}

float selectedFractionOfThisPixel() {
    float selectedTexels = 0.0;
    for (int y = -1; y <= 1; y += 2) {
        for (int x = -1; x <= 1; x += 2) {
            if (readMask(vec2(x, y) * kTexelOffset).r > kTexelSelected) {
                selectedTexels += 1.0;
            }
        }
    }
    return selectedTexels / 4.0;
}

void main() {
    float selectedFraction = selectedFractionOfThisPixel();
    if (selectedFraction >= 1.0) {
        discard;
    }

    float lineEnd = float(radius) + 0.5;
    float nearestDistance2 = lineEnd * lineEnd;
    bool found = false;
    int searchCells = radius + 1;
    for (int y = -searchCells; y <= searchCells; ++y) {
        for (int x = -searchCells; x <= searchCells; ++x) {
            vec2 cell = vec2(x, y);
            float cellDistance = max(length(cell) - kCellReach, 0.0);
            if (cellDistance * cellDistance >= nearestDistance2 || readMask(cell).r <= kAnyTexelSelected) {
                continue;
            }
            for (int texelY = -1; texelY <= 1; texelY += 2) {
                for (int texelX = -1; texelX <= 1; texelX += 2) {
                    vec2 texel = cell + vec2(texelX, texelY) * kTexelOffset;
                    float distance2 = dot(texel, texel);
                    if (distance2 < nearestDistance2 && readMask(texel).r > kTexelSelected) {
                        nearestDistance2 = distance2;
                        found = true;
                    }
                }
            }
        }
    }

    if (!found) {
        discard;
    }
    float coverage = clamp(lineEnd - sqrt(nearestDistance2), 0.0, 1.0) * (1.0 - selectedFraction);
    hd_FragColor = vec4(outlineColor.rgb * coverage, 1.0 - coverage);
    gl_FragDepth = kLineDepth;
}
