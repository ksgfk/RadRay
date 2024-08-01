add_requires("glfw 3.4", {
    debug = is_mode("debug"),
    configs = { 
        shared = false
    }})

target("radray_window")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    if is_plat("macosx") then 
        add_files("*.mm")
    end
    add_deps("radray_core")
    add_packages("glfw")
