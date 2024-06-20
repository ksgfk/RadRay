target("radray_metal")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core", "metal-cpp")
    add_frameworks("Foundation", "Metal", "QuartzCore", "AppKit")
target_end()