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
    QML_FILES Main.qml UiButton.qml BookmarksPanel.qml PerformancePanel.qml ExportDialog.qml SequenceTimeline.qml
    RESOURCES ${viewer_icons})
qt_add_resources(orbelio_viewer viewer_sample PREFIX "/samples" BASE "${PROJECT_SOURCE_DIR}/samples"
    FILES "${PROJECT_SOURCE_DIR}/samples/guitar/guitar.compressed.ply")
set_target_properties(orbelio_viewer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
