#version 450

// Couleur interpolee recue du vertex shader.
layout(location = 0) in  vec3 fragColor;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(fragColor, 1.0);
}
