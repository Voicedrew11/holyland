#version 450
// A grid of quads over the whole target, from the vertex's number alone (no vertex buffer)
layout(push_constant) uniform Push {
    float z;
    int grid;
    float tile_x, tile_y;
} pc;
layout(location = 0) out vec2 v_uv;
void main() {
    int quad = gl_VertexIndex / 6, corner = gl_VertexIndex % 6;
    vec2 at = vec2(quad % pc.grid, quad / pc.grid);
    vec2 offs[6] = vec2[6](vec2(0, 0), vec2(1, 0), vec2(0, 1), vec2(0, 1), vec2(1, 0), vec2(1, 1));
    vec2 p = (at + offs[corner]) / float(pc.grid);
    v_uv = p * vec2(pc.tile_x, pc.tile_y);
    gl_Position = vec4(p * 2.0 - 1.0, pc.z, 1.0);
}
