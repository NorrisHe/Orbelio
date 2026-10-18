# LichtFeld Studio forward renderer, GPL-3.0-or-later. See UPSTREAM.md.
add_library(lfs_vulkan_rasterizer STATIC
    src/barrier_planner.cpp src/buffer.cpp src/gs_pipeline.cpp src/gs_renderer.cpp src/perf_timer.cpp
    support/rendering/vulkan_wait.cpp)
target_include_directories(lfs_vulkan_rasterizer PUBLIC src support compat)
target_link_libraries(lfs_vulkan_rasterizer PUBLIC volk::volk Vulkan::Headers GPUOpen::VulkanMemoryAllocator)
target_compile_definitions(lfs_vulkan_rasterizer PUBLIC VK_NO_PROTOTYPES NOMINMAX
    VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
target_precompile_headers(lfs_vulkan_rasterizer PRIVATE <volk.h>)
set(LFS_ENABLE_VULKAN_VALIDATION ON)
include(shaders.cmake)
