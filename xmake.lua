if path.absolute(os.projectdir()) == path.absolute(os.scriptdir()) and os.exists("scripts/options.lua") then
	includes("scripts/options.lua")
end
if radray_toolchain then
	for k, v in pairs(radray_toolchain) do
		set_config(k, v)
	end
end

function radray_ext_path() 
    return path.join(os.projectdir(), "ext")
end

rule("radray_no_rtti")
    on_load(function(target) 
        target:add("cxflags", "/GR-", {tools = {"clang_cl", "cl"}})
        target:add("cxflags", "-fno-rtti", "-fno-rtti-data", {tools = {"clang"}})
        target:add("cxflags", "-fno-rtti", {tools = {"gcc"}})
    end)
rule_end()

rule("radray_set_ucrt")
    on_load(function(target)
        if is_mode("debug") then
            target:set("runtimes", "MTd")
        else
            target:set("runtimes", "MT")
        end
    end)
rule_end()

rule("radray_basic_setting")
    on_load(function(target)
        target:set("languages", "cxx20")
        target:set("warnings", "all")
        target:add("rules", "radray_set_ucrt")
        if is_mode("debug") then
            target:set("optimize", "none")
            target:add("cxflags", "/GS", {tools = {"clang_cl", "cl"}})
        else
            target:set("optimize", "aggressive")
            target:add("cxflags", "/GS-", {tools = {"clang_cl", "cl"}})
        end
        target:add("cxflags", "/utf-8", "/EHsc", {tools = {"clang_cl", "cl"}})
        target:add("cxflags", "/Zc:preprocessor", "/Zc:__cplusplus", {tools = {"cl"}})
        if target:is_plat("windows") then
            target:add("defines", "RADRAY_PLATFORM_WINDOWS", {public = true})
        elseif target:is_plat("linux") then
            target:add("defines", "RADRAY_PLATFORM_UNIX", {public = true})
        elseif target:is_plat("macosx") then
            target:add("defines", "RADRAY_PLATFORM_MACOS", {public = true})
        end
        if is_mode("debug") then
            target:add("defines", "RADRAY_DEBUG", {public = true})
        end
        if is_arch("arm64") then
            target:add("vectorexts", "neon")
        elseif is_arch("x64", "x86_64") then
            target:add("vectorexts", "avx", "avx2")
        end
        import("core.project.config")
        target:set("targetdir", path.join(os.projectdir(), "bin", config.mode()))
    end)
rule_end()

add_rules("mode.debug", "mode.release")

option("build_test")
    set_values(true, false)
    set_default(true)
    set_showmenu(true)
option_end()

includes("ext")
includes("src")
