# Product source. Repository siblings contain tooling, tests, documentation, assets and dependencies.
add_subdirectory(core)
if(ORBELIO_BUILD_VULKAN_RENDERER)
    add_subdirectory(gpu/vulkan)
endif()
if(ORBELIO_BUILD_VIEWER)
    add_subdirectory(integration/qt_vulkan)
    add_subdirectory(apps/viewer)
endif()
