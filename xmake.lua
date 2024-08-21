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
option("enable_dxc")
    set_values(true, false)
    set_default(true)
    set_showmenu(true)
option_end()
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
-- 一些该工程维护的包
includes("scripts/packages/*.lua")
-- 编译目标
includes("src")
