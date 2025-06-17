if get_config("enable_vulkan") then
    target("hello_world_vk")
        set_kind("binary")
        add_rules("radray_basic_setting", "radray_app", "radray_copy_shaders_to_bin")
        add_files("*.cpp")
        add_deps("radray_core", "radray_window", "radray_render")
end
