// Native draw's pictures between for models (P2-5): a model part's move from the frame before to this one
// by its own matrices (gsn_native_draw.inc's NdBetween and nd_between, which say what each field is), for
// nd_000.vert and nd_280.vert. With u = 1 - t, the part at the picture's time is R Q(u) S(u) p + T(u):
// the rotation by quaternion slerp (Q(u) the turn from this frame's rotation R to the frame before's, by u
// of its angle), the rest of the matrix (S: the scale) and the translation along the line. between_move is
// how far that is from where the part is in this frame, R (Q(u) S(u) - S) p + (T_P - T) u p.w: exactly 0 at
// t 1 (sin 0 and 1 - cos 0 are 0), so a picture at the frame's own time is the frame.

struct Between {
    vec4 rot[3]; // R, a column each
    vec4 s[3];   // S, a column each
    vec4 ds[3];  // the frame before's S less S
    vec4 axis;   // Q(1): its axis, its angle in w
    vec4 dt;     // the frame before's translation less this frame's
};

vec3 between_move(Between b, vec4 p, float u) {
    vec3 sp = (b.s[0].xyz + b.ds[0].xyz * u) * p.x + (b.s[1].xyz + b.ds[1].xyz * u) * p.y +
              (b.s[2].xyz + b.ds[2].xyz * u) * p.z;
    float phi = b.axis.w * u, h = sin(0.5 * phi);
    vec3 a = b.axis.xyz, c = cross(a, sp);
    // (Q(u) - I) sp, by Rodrigues' formula: sin(phi) a x sp + (1 - cos(phi)) a x (a x sp)
    vec3 inner = sin(phi) * c + (2.0 * h * h) * cross(a, c) + (b.ds[0].xyz * p.x + b.ds[1].xyz * p.y + b.ds[2].xyz * p.z) * u;
    return b.rot[0].xyz * inner.x + b.rot[1].xyz * inner.y + b.rot[2].xyz * inner.z + b.dt.xyz * (u * p.w);
}
