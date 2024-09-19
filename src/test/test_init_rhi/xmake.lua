target("test_init_rhi")
    set_kind("binary")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core", "radray_window", "radray_rhi")

    before_build(function (target) 
        import("scripts.helper", {rootdir = os.projectdir()}).copy_example_shaders(target)
    end)

    before_install(function (target) 
        import("scripts.helper", {rootdir = os.projectdir()}).copy_example_shaders(target)
    end)
