add_library(radrayshaderreflector STATIC
    include/radray/shader_reflector/shader_reflector.h
    src/shader_reflector.cpp)
add_library(RadRay::shaderreflector ALIAS radrayshaderreflector)
set_target_properties(radrayshaderreflector PROPERTIES EXPORT_NAME shaderreflector)
target_include_directories(radrayshaderreflector PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(radrayshaderreflector PUBLIC RadRay::runtime RadRay::shader)
radray_default_compile_flags(radrayshaderreflector)
radray_optimize_flags_library(radrayshaderreflector)
radray_set_build_path(radrayshaderreflector)
if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
