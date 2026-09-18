#version 450

layout(push_constant) uniform Frame {
    uint number;
} frame;

layout(location = 0) out vec4 color;

void main() {
    float x = mod(gl_FragCoord.x - 10.0 * frame.number, 100.0);
    color = vec4(1);
    if (x < 10.0) {
        color = vec4(0);
    }
}
