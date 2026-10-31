add_library(orbelio_viewer_support STATIC app_model.cpp vulkan_host.cpp camera_tools.cpp sequence_player.cpp)
target_include_directories(orbelio_viewer_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbelio_viewer_support PUBLIC orbelio_qt orbelio_vulkan Qt6::QuickControls2)

qt_add_executable(orbelio_viewer main.cpp)
target_link_libraries(orbelio_viewer PRIVATE orbelio_viewer_support)
set(viewer_icon_names Cube Camera CaretDown FrameCorners ArrowCounterClockwise ArrowClockwise
    ArrowSquareOut Pulse BookmarkSimple Trash X FloppyDisk Check Eye EyeSlash Play Pause SkipBack SkipForward)
set(viewer_icons assets/icons/LICENSE)
foreach(icon IN LISTS viewer_icon_names)
    list(APPEND viewer_icons assets/icons/${icon}.png assets/icons/${icon}-amber.png
        assets/icons/${icon}-dark.png)
endforeach()
qt_add_qml_module(orbelio_viewer URI Orbelio VERSION 1.0
    QML_FILES Main.qml UiButton.qml BookmarksPanel.qml PerformancePanel.qml InspectorPanel.qml ExportDialog.qml SequenceTimeline.qml
    RESOURCES ${viewer_icons})
qt_add_resources(orbelio_viewer viewer_sample PREFIX "/samples" BASE "${PROJECT_SOURCE_DIR}/samples"
    FILES "${PROJECT_SOURCE_DIR}/samples/guitar/guitar.compressed.ply")
set_target_properties(orbelio_viewer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
get_target_property(viewer_backend_shader_directory lfs_vulkan_rasterizer LFS_VULKAN_RASTERIZER_SPV_DIR)
add_custom_target(orbelio_viewer_shaders ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:orbelio_viewer>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_PROPERTY:orbelio_vulkan,BINARY_DIR>/compose.spv"
        "$<TARGET_PROPERTY:orbelio_vulkan,BINARY_DIR>/tile_stats.spv"
        "$<TARGET_PROPERTY:orbelio_vulkan,BINARY_DIR>/attributes.spv"
        "$<TARGET_FILE_DIR:orbelio_viewer>/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${viewer_backend_shader_directory}/generated" "$<TARGET_FILE_DIR:orbelio_viewer>/shaders/generated"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${viewer_backend_shader_directory}/radix_sort" "$<TARGET_FILE_DIR:orbelio_viewer>/shaders/radix_sort"
    DEPENDS orbelio_viewer VERBATIM)
