vec3 harmonic(uint first, uint coefficient, uint width, uint temporalDegree, vec2 factors) {
    uint at = first+coefficient*3;
    vec3 value = vec3(p.harmonics.values[at],p.harmonics.values[at+1],p.harmonics.values[at+2]);
    if (temporalDegree>0) {
        uint band=at+width;
        value += vec3(p.harmonics.values[band],p.harmonics.values[band+1],p.harmonics.values[band+2])*factors.x;
    }
    if (temporalDegree>1) {
        uint band=at+2*width;
        value += vec3(p.harmonics.values[band],p.harmonics.values[band+1],p.harmonics.values[band+2])*factors.y;
    }
    return value;
}

vec3 color(uint index, vec3 relative) {
    uint degree = p.controls.y;
    uint width = (degree+1)*(degree+1)*3;
    uint temporalDegree = p.controls.w >> 8;
    bool temporal = (p.controls.w & 4u) != 0;
    uint first = index * (width*(temporalDegree+1) + (temporal ? 1 : 0));
    vec2 factors = vec2(0);
    if (temporalDegree != 0) {
        float phase = p.viewport.w - p.harmonics.values[first+width*(temporalDegree+1)];
        factors = cos(vec2(6.28318530718,12.56637061436)*phase);
    }
    vec3 result = vec3(0.5)+0.2820947918*harmonic(first,0,width,temporalDegree,factors);
    if (degree==0) return max(result,vec3(0));
    // Avoid overflow in normalize's squared length for distant calibrated cameras.
    vec3 v = normalize(relative / max(max(abs(relative.x),abs(relative.y)),abs(relative.z)));
    float x = v.x;
    float y = v.y;
    float z = v.z;
    result += (-0.4886025119*y)*harmonic(first,1,width,temporalDegree,factors);
    result += (0.4886025119*z)*harmonic(first,2,width,temporalDegree,factors);
    result += (-0.4886025119*x)*harmonic(first,3,width,temporalDegree,factors);
    if (degree==1) return max(result,vec3(0));
    result += (1.0925484306*x*y)*harmonic(first,4,width,temporalDegree,factors);
    result += (-1.0925484306*y*z)*harmonic(first,5,width,temporalDegree,factors);
    result += (0.3153915653*(3*z*z-1))*harmonic(first,6,width,temporalDegree,factors);
    result += (-1.0925484306*x*z)*harmonic(first,7,width,temporalDegree,factors);
    result += (0.5462742153*(x*x-y*y))*harmonic(first,8,width,temporalDegree,factors);
    if (degree==2) return max(result,vec3(0));
    result += (-0.5900435899*y*(3*x*x-y*y))*harmonic(first,9,width,temporalDegree,factors);
    result += (2.8906114426*x*y*z)*harmonic(first,10,width,temporalDegree,factors);
    result += (-0.4570457995*y*(5*z*z-1))*harmonic(first,11,width,temporalDegree,factors);
    result += (0.3731763326*z*(5*z*z-3))*harmonic(first,12,width,temporalDegree,factors);
    result += (-0.4570457995*x*(5*z*z-1))*harmonic(first,13,width,temporalDegree,factors);
    result += (1.4453057213*z*(x*x-y*y))*harmonic(first,14,width,temporalDegree,factors);
    result += (-0.5900435899*x*(x*x-3*y*y))*harmonic(first,15,width,temporalDegree,factors);
    return max(result,vec3(0));
}
