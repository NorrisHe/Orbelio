layout(push_constant) uniform Parameters { uint count; uint groups; uint chunks; uint shift; uint activeRadix; } config;
const uint radix = 256;
const uint groupSize = 256;
const uint maskWords = groupSize / 32;
const uint histogramTileGroups = 32;
#ifdef DYNAMIC_SORT
layout(set=0,binding=SORT_CONTROL_BINDING,std430) readonly buffer SortControl { uint liveCount; };
#endif
struct SortParameters { uint count; uint groups; uint chunks; uint shift; uint activeRadix; };
bool validSortKey(uint key) {
#ifdef VISIBLE_SORT
    return key!=0xffffffffu;
#else
    return true;
#endif
}
SortParameters sortParameters() {
#ifdef DYNAMIC_SORT
    uint count = min(liveCount,config.count);
    uint groups = (count+groupSize-1)/groupSize;
    return SortParameters(count,groups,(groups+groupSize-1)/groupSize,config.shift,config.activeRadix);
#else
    return SortParameters(config.count,config.groups,config.chunks,config.shift,config.activeRadix);
#endif
}
uint histogramIndex(uint group, uint bin, uint activeRadix) {
    // Coalesced prefix loads, with each histogram tile fitting in the GPU cache.
    return ((group/histogramTileGroups)*activeRadix+bin)*histogramTileGroups+group%histogramTileGroups;
}
