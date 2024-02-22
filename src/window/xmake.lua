add_requires("ext_glfw", _radray_default_require_config())

target("radray_window")
    add_rules("radray_basic_setting")
    set_kind("static")
    add_files("*.cpp")
    add_deps("radray_core")
    add_packages("ext_glfw", {public = true})
target_end()
