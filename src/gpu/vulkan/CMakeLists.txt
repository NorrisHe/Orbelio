add_library(orbelio_vulkan_api INTERFACE)
target_include_directories(orbelio_vulkan_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbelio_vulkan_api INTERFACE orbelio_core volk::volk Vulkan::Headers)
target_compile_definitions(orbelio_vulkan_api INTERFACE VK_NO_PROTOTYPES)

add_library(orbelio_vulkan STATIC renderer.cpp bootstrap_renderer.cpp static_gs_renderer.cpp
    gaussian_renderer.cpp fdu4d_gs_renderer.cpp vma.cpp)
# VMA stays private: only this library's sources allocate, so vk_mem_alloc.h never reaches consumers.
target_link_libraries(orbelio_vulkan PRIVATE lfs_vulkan_rasterizer GPUOpen::VulkanMemoryAllocator)
target_compile_definitions(orbelio_vulkan PRIVATE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
find_program(ORBELIO_GLSLC glslc REQUIRED)
function(orbelio_compute_shader name definition)
    set(shader_spv "${CMAKE_CURRENT_BINARY_DIR}/${name}.spv")
    add_custom_command(OUTPUT "${shader_spv}"
        COMMAND "${ORBELIO_GLSLC}" "${CMAKE_CURRENT_SOURCE_DIR}/${name}.comp" -o "${shader_spv}"
        DEPENDS "${name}.comp" VERBATIM)
    add_custom_target(orbelio_${name}_shader DEPENDS "${shader_spv}")
    add_dependencies(orbelio_vulkan orbelio_${name}_shader)
    target_compile_definitions(orbelio_vulkan PRIVATE ${definition}="${shader_spv}")
endfunction()
orbelio_compute_shader(compose ORBELIO_COMPOSE_SPV)
orbelio_compute_shader(tile_stats ORBELIO_STATS_SPV)

target_link_libraries(orbelio_vulkan PUBLIC orbelio_vulkan_api)
if(ORBELIO_ENABLE_TRACY)
    target_link_libraries(orbelio_vulkan PRIVATE Tracy::TracyClient)
    target_compile_definitions(orbelio_vulkan PRIVATE ORBELIO_TRACY_ENABLED=1)
endif()
if(NOT MSVC)
    set_source_files_properties(vma.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
