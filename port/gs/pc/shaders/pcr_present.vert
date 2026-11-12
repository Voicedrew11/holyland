#version 450
// A triangle over the whole of the presenter's scaled picture (port/gs/pc/pcr.cpp, PresentScaler):
// pcr_present_scale.frag and pcr_present_sharpen.frag. Each pixel is drawn once, at its middle.
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
