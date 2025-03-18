set_project("RadRay")

-- 编译选项
option("build_test")
    set_values(true, false)
    set_default(true)
    set_showmenu(true)
option_end()
option("enable_d3d12")
    set_values(true, false)
    set_default(true)
    set_showmenu(true)
option_end()
option("enable_metal")
    set_values(true, false)
    set_default(true)
    set_showmenu(true)
option_end()
option("enable_mimalloc")
    set_values(true, false)
    set_default(true)
    set_showmenu(true)
option_end()
option("enable_dxc")
    set_values(true, false)
    set_default(true)
    set_showmenu(true)
option_end()
option("enable_spirv_cross")
    set_values(true, false)
    set_default(false)
    set_showmenu(true)
option_end()

set_policy("build.ccache", false)
set_policy("build.warning", true)

-- 本地 xrepo
add_repositories("radray-xrepo xrepo", {rootdir = os.scriptdir()})
-- 引入全局变量
includes("scripts/setup.lua")
if path.absolute(os.projectdir()) == path.absolute(os.scriptdir()) and os.exists("scripts/options.lua") then
	includes("scripts/options.lua")
end
if radray_toolchain then
	for k, v in pairs(radray_toolchain) do
		set_config(k, v)
	end
end
-- 设置全局环境
add_rules("mode.debug", "mode.release")
if is_plat("windows") then
    set_runtimes(is_mode("debug") and "MDd" or "MD")
end

-- 第三方库
add_requires("fmt_radray 11.1.4", {
    debug = is_mode("debug"),
    configs = {
        shared = false,
        header_only = false
    }
})
add_requires("spdlog_radray v1.15.1", {
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
add_requireconfs("spdlog_radray.fmt_radray", {
    version = "11.1.4",
    debug = is_mode("debug"),
    configs = {
        shared = false,
        header_only = false
    }
})
add_requires("eigen 3.4.0")
if get_config("enable_mimalloc") then
    add_requires("mimalloc_radray v2.2.2", {debug = is_mode("debug"), configs = {shared = false}}) 
end
add_requires("xxhash v0.8.3", {debug = is_mode("debug"), configs = {shared = false, dispatch = true}})
add_requires("glfw 3.4", {
    debug = is_mode("debug"),
    configs = { 
        shared = false
    }})
if get_config("enable_d3d12") then
    add_requires("directx-headers v1.615.0", {debug = is_mode("debug")})
    add_requires("d3d12-memory-allocator_radray v2.0.1", {debug = is_mode("debug")})
end
if get_config("enable_dxc") then
    add_requires("directxshadercompiler_radray v1.8.2502")
end
if get_config("enable_metal") then 
    add_requires("metal-cpp macOS15_iOS18", {debug = is_mode("debug")})
end
if get_config("enable_spirv_cross") then
    add_requires("spirv-cross_radray 1.3.296", {debug = is_mode("debug")})
end
if get_config("build_test") then
    add_requires("gtest v1.15.2", {debug = is_mode("debug")})
end

-- 编译目标
includes("src")
