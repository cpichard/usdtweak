#version 330 core
//
// Near and far gives 2 points on the camera ray for a particular fragment, we can then compute a so ray intersection with the
// plane in the fragment shader. Using this ray, [pos = near + alpha*(far-near)] to compute the intersection with the ground plane
// (pos.y=0 or pos.z=0) The position of the plane on the ray is then alpha = -near.y/(far.y-near.y) alpha < 0 means it intersects
// behind the camera, so we won't see it in front, so we can discard the fragment
//
// The depth of the scene rendered by Hydra is read back and the grid composited which gives a soft transition 
// between the grid and the geometry.
//
out vec4 FragColor;
in vec3 near;
in vec3 far;
in mat4 model;
uniform float planeOrientation;
uniform mat4 inverseProjection;
uniform sampler2D sceneDepth;
uniform bool useSceneDepth;
// World size of a pixel on the plane at a LOD. see Grid.cpp.
uniform float lodPixelSize;
// Center of the patches of all the levels, in plane coordinates: the point at the level distance in front of the camera,
// projected on the plane
uniform vec2 patchCenter;
// In orthographic the grid isn't made of finite patches, it fades with the angle between the view and the plane
uniform bool isPerspective;

// Width of the transition where the grid meets the geometry, as a fraction of the distance to the camera
const float contactFadeFraction = 0.002;

// Depth from which a fragment is considered empty. Hydra clears its depth to 1, but the value composited into the viewport
// has lost a little precision: 0.999999881 (1 - 2^-23) was measured on macOS, which made the empty background look like
// geometry sitting on the far plane and hid the grid beyond it. Some margin is kept for other drivers, 1 - 2^-21. Real
// geometry gets that close to 1 only in the farthest percent of the clipping range with a far/near ratio above 2e4.
const float emptyDepth = 1.0 - 1.0 / 2097152.0;

// Distance along the camera axis of the scene geometry under this fragment, or -1 where there is none
float SceneViewDistance() {
    float depth = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (depth >= emptyDepth) {
        return -1.0; // cleared depth, no geometry
    }
    vec2 ndc = gl_FragCoord.xy / vec2(textureSize(sceneDepth, 0)) * 2.0 - 1.0;
    vec4 viewPoint = inverseProjection * vec4(ndc, depth * 2.0 - 1.0, 1.0);
    return -viewPoint.z / viewPoint.w;
}

// Subdivision levels. The cells are powers of 10 world units, chosen for the whole frame from the level distance, see
// lodPixelSize: the smallest level drawn is the first power of 10 with cells at least minCellPixels wide at that
// distance, seen face on. Finer cells appear as the camera gets closer to the plane, coarser ones as it moves away. The
// plane is generally seen at an angle and the cells get smaller with distance, hence a minimum larger than the lines
// need.
const float minCellPixels = 20.0;
// Away from the camera the cells of a level get smaller on screen, the level fades out between these sizes in pixels
// so the distance doesn't turn into a solid mesh: the fine levels dissolve and the coarse ones remain. A level with cells
// 10 times smaller reaches these sizes 10 times closer to the camera, so the lower they are, the further the subgrid
// lasts compared to the main grid. Below about 4 pixels the lines start to merge and can show moire.
const float densityFadeEndPixels = 0.5;
const float densityFadeStartPixels = 1.0;
// Number of levels drawn: the subgrid, the main grid 10 times larger, which is the one generally seen, and two coarser
// levels which carry the grid further away. See LevelIntensity.
const int levelCount = 3;
// In perspective each level is a finite patch, like in Blender, centered on patchCenter, spanning patchHalfCells cells
// of the level on each side of it, and fading out from patchFadeStart of that extent to its edge. A finer level covers
// a smaller area, so the detail decreases away from what is looked at, and the extent of the whole grid is the patch of
// the coarsest level. The grid ends at a finite distance instead of running to the horizon, where the floating point
// precision is lost; it can still come close to the horizon, where the derivatives are not reliable, see the axis lines
// in main. Blender draws 151 lines per level, 75 on each side.
const float patchHalfCells = 75.0;
const float patchFadeStart = 0.5;
// In orthographic the whole grid fades out as the plane is seen edge on, between these sines of the angle between the
// view and the plane.
const float grazingFadeStart = 0.002;
const float grazingFadeEnd = 0.04;

// Fade of the patch of a level of cell size cellSize at the point gridCoord on the plane
float PatchFade(vec2 gridCoord, float cellSize) {
    if (!isPerspective) {
        return 1.0;
    }
    float radius = length(gridCoord - patchCenter) / (patchHalfCells * cellSize);
    return 1.0 - smoothstep(patchFadeStart, 1.0, radius);
}

// Fade of the whole grid with the angle between the view and the plane, in orthographic only
float EdgeOnFade(float grazing) {
    return isPerspective ? 1.0 : smoothstep(grazingFadeStart, grazingFadeEnd, grazing);
}

// Intensity of the minor lines once they are fully faded in, the major lines go up to 1
const float minorLineIntensity = 0.6;

// The style of a level only depends on its scale: the log10 of its cell size relative to the smallest visible cell
// size, greater than 0 and up to levelCount. As the camera moves away the scale of every level decreases, and when it
// crosses an integer the levels shift by one with the same scales, so every line keeps its style. Inside the patches of
// the finer levels a coarse line is also a line of those levels and takes the style of the strongest, see main. Only one level is highlighted, the
// one at a scale between 1 and 2, the coarser levels which carry the grid further away keep the subgrid style.

// How much a level is the highlighted main grid rather than a subgrid or coarser level: 1 at a scale of 2, 0 at 1
// and 3.
float LevelMajorness(float scale) {
    return clamp(1.0 - abs(scale - 2.0), 0.0, 1.0);
}

// Intensity of the lines of a level. The finest level fades in over the first half of a decade, the coarsest fades
// out over its last decade, which keeps the coarse lines left in the distance discreet, and the highlighted level is
// at full intensity.
float LevelIntensity(float scale) {
    float fadeIn = min(2.0 * scale, 1.0);
    float fadeOut = clamp(float(levelCount) - scale, 0.0, 1.0);
    return fadeIn * fadeOut * mix(minorLineIntensity, 1.0, LevelMajorness(scale));
}

// Width in pixels and grey level of the subgrid lines and of the main grid lines. A level goes from the first to the
// second as the level becomes the highlighted main grid, see LevelMajorness.
const float minorLineWidth = 1.0;
const float majorLineWidth = 2.0;
const float minorLineGrey = 0.55;
const float majorLineGrey = 0.9;

// Anti aliased line profile, 1 on a line of width widthPixels centered on distancePixels == 0, fading to 0 over a pixel
vec2 LineProfile(vec2 distancePixels, float widthPixels) {
    return 1.0 - smoothstep(vec2(widthPixels * 0.5 - 0.5), vec2(widthPixels * 0.5 + 0.5), distancePixels);
}

// Anti aliased lines of a square grid of cell size cellSize, 0 outside of the lines. d is the size of a pixel on the
// plane along both axes.
float GridLines(vec2 coord, float cellSize, vec2 d, float widthPixels) {
    vec2 offset = mod(coord, cellSize);
    vec2 line = LineProfile(min(offset, cellSize - offset) / d, widthPixels);
    return max(line.x, line.y);
}

void main()
{
    float distanceToPlane = mix(-near.z/(far.z-near.z), -near.y/(far.y-near.y), planeOrientation);
    // pointOnPlane is the actual point on the plane
    vec3 pointOnPlane = near + distanceToPlane * (far - near);
    vec2 gridCoord = mix(pointOnPlane.xy, pointOnPlane.xz, planeOrientation);
    // We need the sum of the absolute value of derivatives in x and y to get the same projected line width.
    // Derivatives are undefined after a discard that depends on the fragment, so they are computed before any.
    vec2 gridCoordDerivative = fwidth(gridCoord);
    // Only the plane strictly in front of the camera is drawn. A distance of 0 means the eye is on the plane, which is
    // then seen edge on, and a ray parallel to the plane gives an infinite or undefined distance. On the horizon the
    // derivatives mix fragments on the plane with fragments that are not, and can't be used either.
    if (!(distanceToPlane > 0.0) || any(isinf(gridCoordDerivative)) || any(isnan(gridCoordDerivative))) discard;
    // Orthographic edge on fade, shared by the grid and the axis lines
    vec3 rayDirection = normalize(far - near);
    float grazing = abs(mix(rayDirection.z, rayDirection.y, planeOrientation));
    float edgeOnFade = EdgeOnFade(grazing);

    // Subdivision levels. lod is the log10 of the smallest cell size which is still minCellPixels wide at the level
    // distance, it is the same for the whole frame.
    float lod = log(max(lodPixelSize, 1e-30) * minCellPixels) / log(10.0);
    float levelFade = fract(lod);
    float cellSize = pow(10.0, floor(lod) + 1.0);
    // Size of a pixel on the plane at this fragment, the larger derivative is used so the cells are not too dense in the
    // direction foreshortened by perspective
    float pixelSize = max(max(gridCoordDerivative.x, gridCoordDerivative.y), 1e-30);
    // Level k has a cell size of 10^k times the smallest cell, its scale relative to a cell of minCellPixels is
    // k + 1 - levelFade. A coarse line is also a line of all the finer levels, the max keeps the brightest, which makes
    // the intensity of every line continuous when lod crosses an integer and the levels shift.
    // A fragment on the lines of several levels takes the largest alpha, and the grey of the most highlighted of those
    // levels. Both are maximums of values continuous with the camera position, so they are continuous too. Picking the
    // grey of the level with the largest alpha instead would jump where two levels have the same alpha.
    float gridLineAlpha = 0.0;
    //float gridLineMajorness = 0.0;
    // Largest alpha a line of any level could have at this fragment, see the axis lines below
    float levelVisibility = 0.0;
    float levelVisibilityAxis = 0.0;
    for (int level = 0; level < levelCount; ++level) {
        float scale = float(level) + 1.0 - levelFade;
        float majorness = LevelMajorness(scale);
        float densityFade = smoothstep(densityFadeEndPixels, densityFadeStartPixels, cellSize / pixelSize);
        float width = mix(minorLineWidth, majorLineWidth, majorness);
        float fade = densityFade * PatchFade(gridCoord, cellSize);
        float visibility = fade * LevelIntensity(scale);
        float line = GridLines(gridCoord, cellSize, gridCoordDerivative, width);
        gridLineAlpha = max(gridLineAlpha, line * visibility);
        //gridLineMajorness = 0.0;//max(gridLineMajorness, line * fade * majorness);
        levelVisibility = max(levelVisibility, visibility);
        levelVisibilityAxis = fade * 0.8;
        cellSize *= 10.0;
    }
    // Test with 3 level of brightness
    //vec4 c = vec4(vec3(mix(minorLineGrey, majorLineGrey, gridLineMajorness)), gridLineAlpha * edgeOnFade);
    vec4 c = vec4(vec3(minorLineGrey), gridLineAlpha * edgeOnFade);
    // Axis lines: the grid lines at coordinate 0, drawn in the color of the world axis they follow.
    // gridCoord.y == 0 is the X axis, gridCoord.x == 0 is the Z axis on a Y-up stage (xz plane) and the Y axis on a Z-up
    // stage (xy plane). Same line profile and width as the main grid lines.
    vec2 axisLine = LineProfile(abs(gridCoord) / gridCoordDerivative, majorLineWidth);
    // An axis line is a line of every level and fades like the strongest of them: it ends where the grid ends. This also
    // removes it next to the horizon, where the derivatives are wrong and so large that the whole rows of fragments
    // there would pass the test above and be drawn in the axis colors; the density fade of every level is 0 there.
    axisLine *= edgeOnFade * levelVisibilityAxis;
    const vec3 xAxisColor = vec3(0.9, 0.25, 0.25);
    vec3 planeAxisColor = mix(vec3(0.3, 0.8, 0.3), vec3(0.3, 0.45, 1.0), planeOrientation);
    if (axisLine.y > 0 && axisLine.y >= axisLine.x) {
        c = vec4(xAxisColor, axisLine.y);
    } else if (axisLine.x > 0) {
        c = vec4(planeAxisColor, axisLine.x);
    }

    // Hide the grid behind the geometry
    if (useSceneDepth && c.a > 0) {
        float sceneDistance = SceneViewDistance();
        if (sceneDistance >= 0.0) {
            float planeDistance = -(model * vec4(pointOnPlane, 1.0)).z;
            float fadeWidth = contactFadeFraction * planeDistance;
            c.a *= smoothstep(-fadeWidth, fadeWidth, sceneDistance - planeDistance);
        }
    }
    if (c.a == 0) discard;
    FragColor = c;
}
