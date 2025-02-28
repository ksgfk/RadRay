target("radray_core")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_includedirs(path.join(os.projectdir(), "include"), {public = true})
    add_files("*.cpp")
    add_packages("spdlog", "xxhash")
    if get_config("enable_mimalloc") then
        add_defines("RADRAY_ENABLE_MIMALLOC", {public = true})
        add_packages("mimalloc_radray", {public = true})
    end
    add_packages("fmt_radray", "eigen", {public = true})
