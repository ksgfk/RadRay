file(GLOB_RECURSE RADRAY_SHADER_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_SHADER_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radrayshader STATIC ${RADRAY_SHADER_SRC} ${RADRAY_SHADER_INC})
add_library(RadRay::shader ALIAS radrayshader)
set_target_properties(radrayshader PROPERTIES EXPORT_NAME shader)
target_include_directories(radrayshader PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(radrayshader PUBLIC radraycore)
radray_default_compile_flags(radrayshader)
radray_optimize_flags_library(radrayshader)
radray_set_build_path(radrayshader)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
