#version 460
#extension GL_GOOGLE_include_directive : require
#include "native_splat.glsl"
layout(set=0,binding=0,std430) readonly buffer Indices { uint indices[]; };
#ifdef DEPTH_OUTPUT
layout(set=0,binding=1,std430) readonly buffer DepthKeys { uint depthKeys[]; };
layout(location=4) flat out float cameraDepth;
#endif
#ifdef NORMAL_OUTPUT
#ifdef DEPTH_OUTPUT
#define NORMAL_BINDING 2
#else
#define NORMAL_BINDING 1
#endif
layout(set=0,binding=NORMAL_BINDING,std430) readonly buffer Normals { vec4 normals[]; };
layout(location=5) flat out vec3 worldNormal;
#endif
layout(push_constant) uniform Parameters { Projected projected; vec2 viewport; } p;
layout(location=0) flat out vec2 mean;
layout(location=1) flat out vec4 conicOpacity;
layout(location=2) flat out vec3 color;
layout(location=3) flat out float cutoffPower;
const float coverageGuard = 1.0/16.0;
void main() {
#ifdef DEPTH_OUTPUT
    cameraDepth = uintBitsToFloat(depthKeys[gl_InstanceIndex]);
#endif
    ProjectedSplat splat = p.projected.values[indices[gl_InstanceIndex]];
    if (splat.color.w < 0) {
        gl_Position = vec4(2,2,2,1);
        mean = vec2(0);
        conicOpacity = vec4(0);
        color = vec3(0);
        cutoffPower = 0;
#ifdef NORMAL_OUTPUT
        worldNormal = vec3(0);
#endif
        return;
    }
    vec2 corner = vec2((gl_VertexIndex&1)*2-1, (gl_VertexIndex>>1)*2-1);
    vec2 pixel;
    if (all(lessThan(splat.rectangle.zw, 2*p.viewport))) {
        // An inverse-whitened square bounds the ellipse without the empty AABB corners.
        // Fixed-function subpixel rounding must not remove pixels inside the analytic ellipse.
        float xExtent = splat.rectangle.z+coverageGuard;
        float yExtent = (splat.rectangle.z*splat.conicOpacity.x)/splat.conicOpacity.z +
            coverageGuard*(1+abs(splat.conicOpacity.y));
        vec2 offset = vec2(corner.x*xExtent,corner.y*yExtent);
        offset.y += splat.conicOpacity.y*offset.x;
        pixel = splat.rectangle.xy + offset;
    } else {
        // Keep extreme coordinates out of fixed-function clipping without changing the Gaussian.
        pixel = clamp(splat.rectangle.xy + corner*(splat.rectangle.zw+coverageGuard), vec2(0), p.viewport);
    }
    gl_Position = vec4(2*pixel/p.viewport-1,0,1);
    mean = splat.rectangle.xy;
    conicOpacity = splat.conicOpacity;
    color = splat.color.rgb;
    cutoffPower = splat.color.w;
#ifdef NORMAL_OUTPUT
    worldNormal = normals[indices[gl_InstanceIndex]].xyz;
#endif
}
