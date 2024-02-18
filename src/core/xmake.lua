add_requires("ext_eigen", "ext_spdlog", _radray_default_require_config())

target("radray_core")
    add_rules("radray_basic_setting")
    set_kind("static")
    add_includedirs(path.join(os.projectdir(), "include"), {public = true})
    add_files("*.cpp")
    add_packages("ext_eigen", "ext_spdlog", {public = true})
target_end()
