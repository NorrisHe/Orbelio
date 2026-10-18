# Product source. Repository siblings contain tooling, tests, documentation, assets and dependencies.
add_subdirectory(core)
if(ORBELIO_BUILD_VIEWER)
    add_subdirectory(gpu/vulkan)
    add_subdirectory(integration/qt_vulkan)
    add_subdirectory(apps/viewer)
endif()
