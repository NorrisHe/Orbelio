add_library(orbelio_vulkan_api INTERFACE)
target_include_directories(orbelio_vulkan_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbelio_vulkan_api INTERFACE orbelio_core volk::volk Vulkan::Headers)
target_compile_definitions(orbelio_vulkan_api INTERFACE VK_NO_PROTOTYPES)

add_library(orbelio_vulkan STATIC renderer.cpp bootstrap_renderer.cpp native_gaussian_renderer.cpp
    native_gpu_resources.cpp native_radix_sort.cpp fdu4d_gs_renderer.cpp vma.cpp)
# VMA stays private: only this library's sources allocate, so vk_mem_alloc.h never reaches consumers.
target_link_libraries(orbelio_vulkan PRIVATE GPUOpen::VulkanMemoryAllocator)
target_compile_definitions(orbelio_vulkan PRIVATE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
find_program(ORBELIO_GLSLC glslc REQUIRED)
set(native_shaders native_project.comp native_histogram.comp native_prefix.comp native_offsets.comp
    native_scatter.comp native_compose.comp native_quad.vert native_quad.frag)
foreach(shader IN LISTS native_shaders)
    set(shader_spv "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
    add_custom_command(OUTPUT "${shader_spv}"
        COMMAND "${ORBELIO_GLSLC}" --target-env=vulkan1.2 -I "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${shader}" -o "${shader_spv}"
        DEPENDS "${shader}" native_splat.glsl native_sort_parameters.glsl native_sort_votes.glsl VERBATIM)
    list(APPEND native_shader_binaries "${shader_spv}")
endforeach()
add_custom_target(orbelio_native_shaders DEPENDS ${native_shader_binaries})
add_dependencies(orbelio_vulkan orbelio_native_shaders)
set_property(TARGET orbelio_vulkan PROPERTY ORBELIO_SHADER_BINARIES "${native_shader_binaries}")
target_compile_definitions(orbelio_vulkan PRIVATE ORBELIO_NATIVE_SHADER_DIR="${CMAKE_CURRENT_BINARY_DIR}")

target_link_libraries(orbelio_vulkan PUBLIC orbelio_vulkan_api)
if(ORBELIO_ENABLE_TRACY)
    target_link_libraries(orbelio_vulkan PRIVATE Tracy::TracyClient)
    target_compile_definitions(orbelio_vulkan PRIVATE ORBELIO_TRACY_ENABLED=1)
endif()
if(NOT MSVC)
    set_source_files_properties(vma.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()
