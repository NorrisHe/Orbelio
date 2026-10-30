#version 460
#extension GL_GOOGLE_include_directive : require
#include "native_splat.glsl"
layout(set=0,binding=0,std430) readonly buffer Indices { uint indices[]; };
layout(push_constant) uniform Parameters { Projected projected; vec2 viewport; } p;
layout(location=0) flat out vec2 mean;
layout(location=1) flat out vec4 conicOpacity;
layout(location=2) flat out vec3 color;
void main() {
    ProjectedSplat splat = p.projected.values[indices[gl_InstanceIndex]];
    if (splat.color.w < 0) {
        gl_Position = vec4(2,2,2,1);
        mean = vec2(0);
        conicOpacity = vec4(0);
        color = vec3(0);
        return;
    }
    vec2 corner = vec2((gl_VertexIndex&1)*2-1, (gl_VertexIndex>>1)*2-1);
    vec2 pixel = splat.rectangle.xy + corner*splat.rectangle.zw;
    gl_Position = vec4(2*pixel/p.viewport-1,0,1);
    mean = splat.rectangle.xy;
    conicOpacity = splat.conicOpacity;
    color = splat.color.rgb;
}
