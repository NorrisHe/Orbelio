#include "native_splat.glsl"
#include "native_tile_control.glsl"
layout(push_constant) uniform TileParameters {
    Projected projected;
    uvec2 viewport;
    uint count;
    uint capacity;
    uint columns;
    uint options;
} p;

uint binningScale() { return (p.options>>2)&3u; }
uint binningShift() { return tileShift+binningScale(); }
uint binningWidth() { return tileWidth<<binningScale(); }
