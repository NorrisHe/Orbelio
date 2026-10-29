add_library(orbelio_core STATIC src/byte_pool.cpp src/static_scene.cpp src/scene_loader.cpp src/camera.cpp
    src/model_decode_common.cpp src/model_decoder.cpp src/read_ply.cpp src/read_splat.cpp
    src/read_ksplat.cpp src/read_spz.cpp src/read_sog.cpp src/model_sog_assets.cpp src/ply_sequence.cpp
    src/read_fdu4d.cpp src/fdu4d_scene.cpp)
target_include_directories(orbelio_core PUBLIC include)
target_link_libraries(orbelio_core PUBLIC orbelio_options)

find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(LibArchive REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(jsoncpp CONFIG REQUIRED)
target_link_libraries(orbelio_core PRIVATE ZLIB::ZLIB zstd::libzstd_shared LibArchive::LibArchive WebP::webp JsonCpp::JsonCpp)
