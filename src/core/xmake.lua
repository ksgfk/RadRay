add_requires("ext_eigen")

target("radray_core")
    add_rules("radray_basic_setting")
    set_kind("static")
    add_includedirs(path.join(os.projectdir(), "include"), {public = true})
    add_files("*.cpp")
    add_packages("ext_eigen")
target_end()
