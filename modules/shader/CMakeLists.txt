file(GLOB_RECURSE RADRAY_SHADER_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_SHADER_SRC CONFIGURE_DEPENDS "src/*.cpp")
list(REMOVE_ITEM RADRAY_SHADER_SRC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dxc.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/spvc.cpp")

if (RADRAY_ENABLE_DXC)
    list(APPEND RADRAY_SHADER_SRC "src/dxc.cpp")
endif()
if (RADRAY_ENABLE_SPIRV_CROSS)
    list(APPEND RADRAY_SHADER_SRC "src/spvc.cpp")
endif()

add_library(radrayshader STATIC ${RADRAY_SHADER_SRC} ${RADRAY_SHADER_INC})
target_include_directories(radrayshader PUBLIC include)
target_link_libraries(radrayshader PUBLIC radraycore)
target_link_libraries(radrayshader PRIVATE
    xxHash::xxhash
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:dxc::dxcompiler>
    $<$<AND:$<BOOL:${RADRAY_ENABLE_DXC}>,$<PLATFORM_ID:Windows>>:Microsoft::DirectX-Headers>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-core>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-glsl>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-msl>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-util>)
target_compile_definitions(radrayshader PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:RADRAY_ENABLE_DXC>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:RADRAY_ENABLE_SPIRV_CROSS>)
radray_default_compile_flags(radrayshader)
radray_optimize_flags_library(radrayshader)
radray_set_build_path(radrayshader)

if (RADRAY_BUILD_SHADER_COOKER)
    add_subdirectory("${CMAKE_SOURCE_DIR}/tools/shader_cooker" "${CMAKE_BINARY_DIR}/tools/shader_cooker")
endif()

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
