file(GLOB_RECURSE RADRAY_SHADER_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_SHADER_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radrayshader STATIC ${RADRAY_SHADER_SRC} ${RADRAY_SHADER_INC})
target_include_directories(radrayshader PUBLIC include)
target_link_libraries(radrayshader PUBLIC radraycore)
radray_default_compile_flags(radrayshader)
radray_optimize_flags_library(radrayshader)
radray_set_build_path(radrayshader)
