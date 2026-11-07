add_library(orbelio_vulkan_api INTERFACE)
target_include_directories(orbelio_vulkan_api INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbelio_vulkan_api INTERFACE orbelio_core volk::volk Vulkan::Headers)
target_compile_definitions(orbelio_vulkan_api INTERFACE VK_NO_PROTOTYPES)

add_library(orbelio_vulkan STATIC renderer.cpp bootstrap_renderer.cpp native_gaussian_renderer.cpp
    native_gpu_resources.cpp native_radix_sort.cpp native_render_targets.cpp native_capture.cpp native_tile_raster.cpp
    fdu4d_gs_renderer.cpp vma.cpp)
# VMA stays private: only this library's sources allocate, so vk_mem_alloc.h never reaches consumers.
target_link_libraries(orbelio_vulkan PRIVATE GPUOpen::VulkanMemoryAllocator)
target_compile_definitions(orbelio_vulkan PRIVATE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
find_program(ORBELIO_GLSLC glslc REQUIRED)
set(native_shaders native_project.comp native_shading.comp native_histogram.comp native_prefix.comp native_offsets.comp
    native_scatter.comp native_compose.comp native_quad.vert native_quad.frag
    native_tile_source_dispatch.comp native_tile_bin.comp native_tile_cooperative.comp native_tile_cooperative_dispatch.comp native_tile_scan.comp native_tile_emit.comp native_tile_dispatch.comp native_tile_ranges.comp native_tile_raster.comp)
foreach(shader IN LISTS native_shaders)
    set(shader_spv "${CMAKE_CURRENT_BINARY_DIR}/${shader}.spv")
    add_custom_command(OUTPUT "${shader_spv}"
        COMMAND "${ORBELIO_GLSLC}" --target-env=vulkan1.2 -I "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_SOURCE_DIR}/${shader}" -o "${shader_spv}"
        DEPENDS "${shader}" native_splat.glsl native_alpha.glsl native_project_parameters.glsl native_sh.glsl
            native_sort_parameters.glsl native_sort_votes.glsl native_tile_control.glsl native_tile_parameters.glsl native_tile_intersection.glsl VERBATIM)
    list(APPEND native_shader_binaries "${shader_spv}")
endforeach()
foreach(variant IN ITEMS native_quad_depth.vert native_quad_depth.frag native_compose_float.comp
    native_project_normals.comp native_quad_normals.vert native_quad_normals.frag
    native_quad_depth_normals.vert native_quad_depth_normals.frag
    native_histogram_dynamic.comp native_prefix_dynamic.comp native_offsets_dynamic.comp native_scatter_dynamic.comp
    native_tile_raster_diagnostics.comp native_tile_raster_macro.comp native_tile_raster_macro_diagnostics.comp
    native_tile_raster_normals.comp native_tile_raster_normals_diagnostics.comp
    native_tile_raster_normals_macro.comp native_tile_raster_normals_macro_diagnostics.comp
    native_tile_cooperative_emit.comp
    native_histogram_visible.comp native_offsets_visible.comp native_scatter_visible.comp)
    string(REPLACE "_depth" "" source "${variant}")
    string(REPLACE "_float" "" source "${source}")
    string(REPLACE "_dynamic" "" source "${source}")
    string(REPLACE "_visible" "" source "${source}")
    string(REPLACE "_diagnostics" "" source "${source}")
    string(REPLACE "_macro" "" source "${source}")
    string(REPLACE "_normals" "" source "${source}")
    if(variant MATCHES "cooperative_emit")
        set(source native_tile_cooperative.comp)
    endif()
    if(variant MATCHES "_macro_diagnostics")
        set(variant_define TILE_MACRO_DIAGNOSTICS)
    elseif(variant MATCHES "_macro")
        set(variant_define TILE_MACRO)
    elseif(variant MATCHES "_depth")
        set(variant_define DEPTH_OUTPUT)
    elseif(variant MATCHES "_float")
        set(variant_define FLOAT_OUTPUT)
    elseif(variant MATCHES "cooperative_emit")
        set(variant_define COOPERATIVE_EMIT)
    elseif(variant MATCHES "_visible")
        set(variant_define VISIBLE_SORT)
    elseif(variant MATCHES "_diagnostics")
        set(variant_define TILE_DIAGNOSTICS)
    elseif(variant MATCHES "_normals")
        set(variant_define NORMAL_OUTPUT)
    else()
        set(variant_define DYNAMIC_SORT)
    endif()
    set(variant_defines -D${variant_define})
    if(variant MATCHES "_normals" AND NOT variant_define STREQUAL "NORMAL_OUTPUT")
        list(APPEND variant_defines -DNORMAL_OUTPUT)
    endif()
    set(shader_spv "${CMAKE_CURRENT_BINARY_DIR}/${variant}.spv")
    add_custom_command(OUTPUT "${shader_spv}"
        COMMAND "${ORBELIO_GLSLC}" --target-env=vulkan1.2 ${variant_defines}
            -I "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/${source}" -o "${shader_spv}"
        DEPENDS "${source}" native_splat.glsl native_alpha.glsl native_sort_parameters.glsl native_sort_votes.glsl
            native_tile_control.glsl native_tile_parameters.glsl native_tile_intersection.glsl VERBATIM)
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
