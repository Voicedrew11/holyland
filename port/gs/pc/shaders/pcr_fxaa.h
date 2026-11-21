// FXAA (Timothy Lottes, NVIDIA, 2009; its "quality" form), written here from the method's published description, for
// pcr_fxaa.frag (the picture's pass) and pcr_present_fused.comp (the final pass in one): where the luma's contrast
// around a pixel is high, find the edge's direction (across or down), walk along it both ways to its ends, and read the
// picture part of a pixel towards the edge's other side by how near the pixel is to the edge's nearer end; on a thin or
// noisy feature, by its contrast with the pixels around instead. Luma from the colours as shown (not linear light), as
// the method has it.
//
// The includer defines FXAA_SRC (a sampler2D of the picture, read bilinear, clamped) and FXAA_TEXEL (a vec2: 1 / the
// picture's size).
const float EDGE_MIN = 1.0 / 24.0; // (no edge below this contrast, or below EDGE_RATIO of the brightest around)
const float EDGE_RATIO = 1.0 / 8.0;
const float SUBPIXEL = 0.75; // (how much a thin feature is blended with its surround)
const int STEPS = 12;
// (how far each step along the edge goes, in pixels: one at a time near, longer further on)
const float STEP_LENGTH[STEPS] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

float luma(vec3 c) {
    return dot(c, vec3(0.299, 0.587, 0.114));
}

float luma_at(vec2 uv) {
    return luma(textureLod(FXAA_SRC, uv, 0.0).rgb);
}

float luma_near(vec2 uv, ivec2 offset) {
    return luma_at(uv + vec2(offset) * FXAA_TEXEL);
}

// FXAA at the pixel whose middle is uv (FXAA_SRC's coordinates), its texel FXAA_TEXEL: the colour to show
vec3 fxaa_at(vec2 uv) {
    vec3 centre = textureLod(FXAA_SRC, uv, 0.0).rgb;
    float m = luma(centre);
    float up = luma_near(uv, ivec2(0, -1)), down = luma_near(uv, ivec2(0, 1));
    float left = luma_near(uv, ivec2(-1, 0)), right = luma_near(uv, ivec2(1, 0));
    float lo = min(m, min(min(up, down), min(left, right)));
    float hi = max(m, max(max(up, down), max(left, right)));
    float range = hi - lo;
    if (range < max(EDGE_MIN, hi * EDGE_RATIO)) {
        return centre;
    }
    float up_left = luma_near(uv, ivec2(-1, -1)), up_right = luma_near(uv, ivec2(1, -1));
    float down_left = luma_near(uv, ivec2(-1, 1)), down_right = luma_near(uv, ivec2(1, 1));
    float up_down = up + down, left_right = left + right;
    float left_corners = up_left + down_left, right_corners = up_right + down_right;
    float up_corners = up_left + up_right, down_corners = down_left + down_right;

    // The edge's direction: across (horizontal: the luma changes going down) or down
    float across = abs(-2.0 * left + left_corners) + 2.0 * abs(-2.0 * m + up_down) + abs(-2.0 * right + right_corners);
    float downward = abs(-2.0 * up + up_corners) + 2.0 * abs(-2.0 * m + left_right) + abs(-2.0 * down + down_corners);
    bool horizontal = across >= downward;

    // Its side: towards the neighbour across it with the larger change
    float before = horizontal ? up : left, after = horizontal ? down : right;
    float change_before = before - m, change_after = after - m;
    bool before_steeper = abs(change_before) >= abs(change_after);
    float threshold = 0.25 * max(abs(change_before), abs(change_after));
    float step_across = horizontal ? FXAA_TEXEL.y : FXAA_TEXEL.x;
    float edge_luma;
    if (before_steeper) {
        step_across = -step_across;
        edge_luma = 0.5 * (before + m);
    } else {
        edge_luma = 0.5 * (after + m);
    }

    // Along the edge (half a pixel over, on it) both ways, until the luma there leaves the edge's
    vec2 on_edge = uv;
    if (horizontal) {
        on_edge.y += 0.5 * step_across;
    } else {
        on_edge.x += 0.5 * step_across;
    }
    vec2 along = horizontal ? vec2(FXAA_TEXEL.x, 0.0) : vec2(0.0, FXAA_TEXEL.y);
    vec2 uv_back = on_edge - along, uv_on = on_edge + along;
    float end_back = luma_at(uv_back) - edge_luma, end_on = luma_at(uv_on) - edge_luma;
    bool done_back = abs(end_back) >= threshold, done_on = abs(end_on) >= threshold;
    for (int i = 1; i < STEPS && !(done_back && done_on); i++) {
        if (!done_back) {
            uv_back -= along * STEP_LENGTH[i];
            end_back = luma_at(uv_back) - edge_luma;
            done_back = abs(end_back) >= threshold;
        }
        if (!done_on) {
            uv_on += along * STEP_LENGTH[i];
            end_on = luma_at(uv_on) - edge_luma;
            done_on = abs(end_on) >= threshold;
        }
    }

    // How far over to read: by the nearer end, if the luma there goes the way the edge does
    float to_back = horizontal ? uv.x - uv_back.x : uv.y - uv_back.y;
    float to_on = horizontal ? uv_on.x - uv.x : uv_on.y - uv.y;
    bool back_nearer = to_back < to_on;
    float nearest = min(to_back, to_on);
    float span = to_back + to_on;
    float edge_offset = 0.5 - nearest / span;
    bool centre_darker = m < edge_luma;
    bool ends_agree = ((back_nearer ? end_back : end_on) < 0.0) != centre_darker;
    float offset = ends_agree ? edge_offset : 0.0;

    // A thin feature: by its contrast with the 3x3 around it
    float around = (2.0 * (up_down + left_right) + left_corners + right_corners) / 12.0;
    float sub = clamp(abs(around - m) / range, 0.0, 1.0);
    sub = (-2.0 * sub + 3.0) * sub * sub;
    offset = max(offset, sub * sub * SUBPIXEL);

    vec2 read_at = uv;
    if (horizontal) {
        read_at.y += offset * step_across;
    } else {
        read_at.x += offset * step_across;
    }
    return textureLod(FXAA_SRC, read_at, 0.0).rgb;
}
