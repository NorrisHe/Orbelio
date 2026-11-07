#include "native_splat.glsl"
layout(set = 0, binding = CAMERA_BINDING, std430) readonly buffer Camera {
    vec4 intrinsics;
    vec4 clipping;
} camera;
layout(push_constant) uniform Parameters {
    vec4 right;
    vec4 down;
    vec4 forward;
    vec4 eye;
    vec4 viewport;
    uvec4 controls;
    Positions positions;
    Orientations orientations;
    Harmonics harmonics;
    Projected projected;
} p;
