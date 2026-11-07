const uint tileWidth = 16;
const uint tileShift = 4;
const uint tileGroupSize = tileWidth*tileWidth;
const uint overflowFlag = 1;
const uint selectedFlag = 2;
const uint reservedCountWord = 12;
const uint rasterDispatchWord = 13;
const uint cooperativeThreshold = 256;
const uint cooperativeCountWord = 28;
const uint cooperativeDispatchWord = 29;
const uint cooperativeEmitWord = 32;
const uint sourceCountWord = 35;
const uint cooperativeTasksWord = 36;
const uint cooperativeDispatchColumns = 65535;

bool cooperativeBounds(uvec4 rectangle) {
    uvec2 span = rectangle.zw-rectangle.xy+1;
    return span.x*span.y>cooperativeThreshold;
}

uvec4 packTileBounds(uvec4 rectangle, uint source, uint columns) {
    return uvec4(rectangle.y*columns+rectangle.x,rectangle.w*columns+rectangle.z,source,1);
}

uvec4 unpackTileBounds(uvec4 encoded, uint columns) {
    return uvec4(encoded.x%columns,encoded.x/columns,encoded.y%columns,encoded.y/columns);
}
