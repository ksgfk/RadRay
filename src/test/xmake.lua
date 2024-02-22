target("test_logger")
    add_rules("radray_basic_setting")
    set_kind("binary")
    add_files("test_logger.cpp")
    add_deps("radray_core")
target_end()


if is_plat("windows") then
    target("test_d3d12_device")
        add_rules("radray_basic_setting")
        set_kind("binary")
        add_files("test_d3d12_device.cpp")
        add_deps("radray_d3d12")
    target_end()
end
