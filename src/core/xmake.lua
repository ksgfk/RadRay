target("radray_core")
    add_rules("radray_basic_setting")
    set_kind("static")
    add_includedirs(path.join(os.projectdir(), "include"), {public = true})
    add_files("*.cpp")
    add_deps("eigen", "spdlog")
target_end()
