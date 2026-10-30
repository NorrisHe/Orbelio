#extension GL_EXT_buffer_reference : require

struct ProjectedSplat {
    vec4 rectangle;
    vec4 conicOpacity;
    vec4 color;
};
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Positions { vec4 values[]; };
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Orientations { vec4 values[]; };
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Harmonics { float values[]; };
#ifdef PROJECT_SPLATS
layout(buffer_reference, std430, buffer_reference_align = 16) buffer Projected { ProjectedSplat values[]; };
#else
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer Projected { ProjectedSplat values[]; };
#endif
