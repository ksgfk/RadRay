add_library(radraygpu STATIC
    src/types.cpp
    src/external_resource.cpp)
add_library(RadRay::gpu ALIAS radraygpu)
set_target_properties(radraygpu PROPERTIES EXPORT_NAME gpu)
target_include_directories(radraygpu PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(radraygpu PUBLIC RadRay::core)
radray_default_compile_flags(radraygpu)
radray_optimize_flags_library(radraygpu)
radray_set_build_path(radraygpu)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
