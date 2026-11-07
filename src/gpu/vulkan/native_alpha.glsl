float gaussianAlpha(vec2 pixel, vec2 mean, vec4 conicOpacity, float cutoffPower) {
    precise vec2 d = pixel-mean;
    precise float x = d.x*conicOpacity.x;
    precise float y = fma(-conicOpacity.y,d.x,d.y)*conicOpacity.z;
    precise float squared = fma(x,x,y*y);
    precise float power = -0.5*squared;
    if (power < -cutoffPower) return 0;
    precise float alpha = min(0.999,conicOpacity.w*exp(power));
    return alpha < 0.5/255.0 ? 0 : alpha;
}
