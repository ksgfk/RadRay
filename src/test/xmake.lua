target("test_buddy_alloc")
    set_kind("binary")
    add_rules("radray_basic_setting")
    add_files("test_buddy_alloc.cpp")
    add_deps("radray_core")
    set_default(false)
    add_tests("default")
target_end()

-- target("test_nri")
--     set_kind("binary")
--     add_rules("radray_basic_setting")
--     add_files("test_nri.cpp")
--     add_deps("radray_core", "radray_window", "nri")
-- target_end()

-- includes("test_init_rhi")
