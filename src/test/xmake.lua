target("test_buddy_alloc")
    set_kind("binary")
    add_rules("radray_basic_setting")
    add_files("test_buddy_alloc.cpp")
    add_deps("radray_core")
target_end()

if get_config("enable_dxc") then 
    target("test_dynamic_lib")
        set_kind("binary")
        add_rules("radray_basic_setting")
        add_files("test_dynamic_lib.cpp")
        add_deps("radray_core")
        before_build(function (target)
            import("scripts.helper", {rootdir = os.projectdir()}).copy_dxc_lib(target)
        end)
        before_build(function (target)
            import("scripts.helper", {rootdir = os.projectdir()}).copy_dxc_lib(target)
        end)
    target_end()
end

-- target("test_nri")
--     set_kind("binary")
--     add_rules("radray_basic_setting")
--     add_files("test_nri.cpp")
--     add_deps("radray_core", "radray_window", "nri")
-- target_end()

-- includes("test_init_rhi")
