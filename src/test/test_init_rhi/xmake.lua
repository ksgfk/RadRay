target("test_init_rhi")
    set_kind("binary")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core", "radray_window", "radray_rhi")
target_end()
