if get_config("enable_d3d12") then
    target("hello_world_dx12")
        set_kind("binary")
        add_rules("radray_basic_setting", "radray_app", "radray_copy_shaders_to_bin")
        add_files("*.cpp")
        add_deps("radray_core", "radray_window", "radray_render")
end
