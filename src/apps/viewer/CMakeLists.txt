add_library(orbelio_viewer_support STATIC app_model.cpp vulkan_host.cpp camera_tools.cpp)
target_include_directories(orbelio_viewer_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(orbelio_viewer_support PUBLIC orbelio_qt orbelio_vulkan Qt6::QuickControls2)

qt_add_executable(orbelio_viewer main.cpp)
target_link_libraries(orbelio_viewer PRIVATE orbelio_viewer_support)
qt_add_qml_module(orbelio_viewer URI Orbelio VERSION 1.0 QML_FILES Main.qml)
qt_add_resources(orbelio_viewer viewer_sample PREFIX "/samples" BASE "${PROJECT_SOURCE_DIR}/samples"
    FILES "${PROJECT_SOURCE_DIR}/samples/guitar/guitar.compressed.ply")
set_target_properties(orbelio_viewer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
