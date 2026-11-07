bool intersectsTileAtWidth(uvec2 tile, ProjectedSplat splat, uint width) {
    precise vec2 low = vec2(tile*width)+0.5-splat.rectangle.xy;
    precise vec2 high = vec2(min((tile+1)*width,p.viewport))-0.5-splat.rectangle.xy;
    precise float x0 = low.x*splat.conicOpacity.x;
    precise float x1 = high.x*splat.conicOpacity.x;
    precise float b = splat.conicOpacity.y;
    precise float y0 = fma(-b,b>=0 ? high.x : low.x,low.y)*splat.conicOpacity.z;
    precise float y1 = fma(-b,b>=0 ? low.x : high.x,high.y)*splat.conicOpacity.z;
    if (isnan(x0) || isnan(x1) || isnan(y0) || isnan(y1)) return true;
    // Independent interval minima underestimate the quadratic, so rejection remains conservative.
    precise float x = max(max(x0,-x1),0);
    precise float y = max(max(y0,-y1),0);
    precise float squared = fma(x,x,y*y);
    return isnan(squared) || -0.5*squared>=-splat.color.w;
}

bool intersectsTile(uvec2 tile, ProjectedSplat splat) {
    return intersectsTileAtWidth(tile,splat,binningWidth());
}

uvec4 clippedTileBounds(ProjectedSplat splat, uint shift) {
    // Pixel slack keeps rounding at the floating-point ellipse boundary conservative.
    vec2 low=clamp(splat.rectangle.xy-splat.rectangle.zw-1,vec2(0),vec2(p.viewport));
    vec2 high=clamp(splat.rectangle.xy+splat.rectangle.zw+1,vec2(0),vec2(p.viewport));
    uvec2 grid=(p.viewport+(1u<<shift)-1)>>shift;
    uvec2 first=uvec2(low)>>shift;
    uvec2 last=min(uvec2(high)>>shift,grid-1);
    return uvec4(first,last);
}
