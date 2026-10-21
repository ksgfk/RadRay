file(GLOB_RECURSE RADRAY_SHADER_COMPILER_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_SHADER_COMPILER_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radrayshadercompiler STATIC ${RADRAY_SHADER_COMPILER_SRC} ${RADRAY_SHADER_COMPILER_INC})
add_library(RadRay::shadercompiler ALIAS radrayshadercompiler)
set_target_properties(radrayshadercompiler PROPERTIES EXPORT_NAME shadercompiler)
target_include_directories(radrayshadercompiler PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(radrayshadercompiler PRIVATE RadRayDXC::Headers)
target_link_libraries(radrayshadercompiler PUBLIC radraycore)
target_link_libraries(radrayshadercompiler PUBLIC radrayshader)
target_compile_definitions(radrayshadercompiler PRIVATE RADRAY_SHADER_COMPILER_FORK)
radray_default_compile_flags(radrayshadercompiler)
radray_optimize_flags_library(radrayshadercompiler)
radray_set_build_path(radrayshadercompiler)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
