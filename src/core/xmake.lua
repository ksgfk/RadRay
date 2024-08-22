add_requires("spdlog_radray v1.14.1", {
    alias = "spdlog",
    debug = is_mode("debug"),
    configs = {
        shared = false,
        header_only = false,
        std_format = false,
        fmt_external = true,
        noexcept = true,
        no_thread_id = true,
        no_default_logger = true
    }})
add_requires("eigen 3.4.0")

target("radray_core")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_includedirs(path.join(os.projectdir(), "include"), {public = true})
    add_files("*.cpp")
    add_packages("spdlog", "eigen", {public = true})
