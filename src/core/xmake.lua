target("radray_core")
    set_kind("static")
    add_rules("c++.unity_build", {batchsize = 32})
    add_rules("radray_basic_setting")
    add_includedirs(path.join(os.projectdir(), "include"), {public = true})
    add_files("*.cpp")
    add_deps("spdlog", "eigen")
target_end()
