target("radray_runtime")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_includedirs(path.join(os.projectdir(), "include"), {public = true})
    set_pcxxheader("pch.h")
    add_files("*.cpp")
    add_deps("radray_core", "radray_render")
