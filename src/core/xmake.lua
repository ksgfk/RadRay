add_requires("fmt 11.0.2", {
    debug = is_mode("debug"),
    configs = {
        shared = false,
        header_only = false
    }
})
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
add_requireconfs("spdlog_radray.fmt", {
    version = "11.0.2",
    debug = is_mode("debug"),
    configs = {
        shared = false,
        header_only = false
    }
})
add_requires("eigen 3.4.0")
if get_config("enable_mimalloc") then
    add_requires("mimalloc v2.1.7", {debug = is_mode("debug"), configs = {shared = false}}) 
end

target("radray_core")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_includedirs(path.join(os.projectdir(), "include"), {public = true})
    add_files("*.cpp")
    add_packages("spdlog")
    if get_config("enable_mimalloc") then
        add_defines("RADRAY_ENABLE_MIMALLOC")
        add_packages("mimalloc")
    end
    add_packages("fmt", "eigen", {public = true})
