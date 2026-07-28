# C++ 工具目录。Python 脚本 (fetch_*, generate_imgui_shader) 不经 cmake, 手动运行。
if (RADRAY_ENABLE_SHADER_JIT)
    add_subdirectory(shader_gen)
    add_subdirectory(shader_cook)
endif()
