target("radray_window")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    if is_plat("macosx") then 
        add_files("*.mm")
    end
    add_deps("radray_core")
    add_packages("glfw")
