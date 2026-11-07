#version 460
#extension GL_GOOGLE_include_directive : require
#include "native_alpha.glsl"
layout(location=0) flat in vec2 mean;
layout(location=1) flat in vec4 conicOpacity;
layout(location=2) flat in vec3 color;
layout(location=3) flat in float cutoffPower;
layout(location=0) out vec4 outputColor;
#ifdef DEPTH_OUTPUT
layout(location=4) flat in float cameraDepth;
layout(location=1) out vec4 outputDepth;
#endif
#ifdef NORMAL_OUTPUT
layout(location=5) flat in vec3 worldNormal;
#ifdef DEPTH_OUTPUT
layout(location=2) out vec4 outputNormal;
#else
layout(location=1) out vec4 outputNormal;
#endif
#endif
void main() {
    float alpha = gaussianAlpha(gl_FragCoord.xy,mean,conicOpacity,cutoffPower);
    if (alpha==0) discard;
    outputColor = vec4(color*alpha,alpha);
#ifdef DEPTH_OUTPUT
    outputDepth = vec4(alpha*cameraDepth,0,0,alpha);
#endif
#ifdef NORMAL_OUTPUT
    outputNormal = vec4(worldNormal*alpha,alpha);
#endif
}
