add_requires("metal-cpp macOS14.2_iOS17.2", {
    debug = is_mode("debug"),
    configs = { 
        shared = false
    }})

target("radray_metal")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_defines("RADRAY_ENABLE_METAL", {public = true})
    add_files("*.cpp")
    add_files("*.mm")
    add_deps("radray_core")
    add_packages("metal-cpp", {public = true})
    add_frameworks("Foundation", "Metal", "QuartzCore", "AppKit")
