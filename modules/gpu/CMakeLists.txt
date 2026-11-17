add_library(radraygpu INTERFACE)
add_library(RadRay::gpu ALIAS radraygpu)
set_target_properties(radraygpu PROPERTIES EXPORT_NAME gpu)
target_include_directories(radraygpu INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(radraygpu INTERFACE RadRay::core)
if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
