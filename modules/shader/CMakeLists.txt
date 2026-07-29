file(GLOB_RECURSE RADRAY_SHADER_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_SHADER_SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(radrayshader STATIC ${RADRAY_SHADER_SRC} ${RADRAY_SHADER_INC})
target_include_directories(radrayshader PUBLIC include)
target_link_libraries(radrayshader PUBLIC radraycore)
target_link_libraries(radrayshader PRIVATE
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:dxc::dxcompiler>
    $<$<AND:$<BOOL:${RADRAY_ENABLE_DXC}>,$<PLATFORM_ID:Windows>>:Microsoft::DirectX-Headers>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-core>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-glsl>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-msl>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:spirv-cross-util>)
target_compile_definitions(radrayshader PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_DXC}>:RADRAY_ENABLE_DXC>
    $<$<BOOL:${RADRAY_ENABLE_SPIRV_CROSS}>:RADRAY_ENABLE_SPIRV_CROSS>
    $<$<BOOL:${RADRAY_ENABLE_SHADER_JIT}>:RADRAY_ENABLE_SHADER_JIT>)

if (NOT DEFINED RADRAY_SDK_dxc_VERSION)
    message(FATAL_ERROR "RADRAY_SDK_dxc_VERSION is required for the shader toolchain hash")
endif()
target_compile_definitions(radrayshader PRIVATE
    RADRAY_DXC_VERSION="${RADRAY_SDK_dxc_VERSION}")
radray_default_compile_flags(radrayshader)
radray_optimize_flags_library(radrayshader)
radray_set_build_path(radrayshader)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if (RADRAY_ENABLE_SHADER_JIT)
    # 先删再拷: copy_directory 不会移除源码树里已删掉的文件, 残留的旧头文件会让重命名后的 #include 悄悄解析到过期副本, 排查起来极其费时。
    set(_RADRAY_SHADER_JIT_DEPLOY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E rm -rf "$<TARGET_FILE_DIR:radrayshader>/shaderlib"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/shaderlib" "$<TARGET_FILE_DIR:radrayshader>/shaderlib")
    foreach(_dll IN LISTS RADRAY_DXC_RUNTIME_DLLS)
        list(APPEND _RADRAY_SHADER_JIT_DEPLOY_COMMANDS
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:radrayshader>")
    endforeach()
    add_custom_command(TARGET radrayshader POST_BUILD
        ${_RADRAY_SHADER_JIT_DEPLOY_COMMANDS}
        COMMENT "Deploy shader sources and DXC runtime for shader JIT")
    unset(_RADRAY_SHADER_JIT_DEPLOY_COMMANDS)
endif()
