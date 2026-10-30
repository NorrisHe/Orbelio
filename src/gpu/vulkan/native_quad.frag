#version 460
layout(location=0) flat in vec2 mean;
layout(location=1) flat in vec4 conicOpacity;
layout(location=2) flat in vec3 color;
layout(location=0) out vec4 outputColor;
void main() {
    vec2 d = gl_FragCoord.xy-mean;
    float power = -0.5*(conicOpacity.x*d.x*d.x + 2*conicOpacity.y*d.x*d.y + conicOpacity.z*d.y*d.y);
    float alpha = min(0.999,conicOpacity.w*exp(power));
    if (alpha < 0.5/255.0) discard;
    outputColor = vec4(color*alpha,alpha);
}
