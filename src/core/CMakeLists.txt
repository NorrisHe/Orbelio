add_library(orbelio_core STATIC src/byte_pool.cpp src/static_scene.cpp src/scene_loader.cpp src/camera.cpp)
target_include_directories(orbelio_core PUBLIC include)
target_link_libraries(orbelio_core PUBLIC orbelio_options)
