function _radray_default_require_config() 
    return {
        debug = is_mode("debug") or false,
        config = {
            vs_runtime = (function() 
                if is_mode("debug") then return "MDd"
                else return "MD" end
            end)()
        },
        external = false
    }
end

rule("radray_basic_setting")
    on_load(function(target)
        target:set("languages", "cxx20")
        target:set("warnings", "all")
        if is_mode("debug") then
            target:set("optimize", "none")
            target:set("runtimes", "MDd")
            target:add("cxflags", "/GS", {tools = {"clang_cl", "cl"}})
        else
            target:set("optimize", "aggressive")
            target:set("runtimes", "MD")
            target:add("cxflags", "/GS-", {tools = {"clang_cl", "cl"}})
            target:add("cxflags", "/GL", {tools = {"clang_cl", "cl", public = true}})
            target:add("ldflags", "/LTCG", {tools = {"clang_cl", "cl", public = true}})
        end
        target:add("cxflags", "/utf-8", "/Gd", "/EHsc", {tools = {"clang_cl", "cl"}})
        target:add("cxflags", "/Zc:preprocessor", "/Zc:__cplusplus", {tools = {"cl"}})
        if target:is_plat("windows") then
            target:add("defines", "RADRAY_PLATFORM_WINDOWS", {public = true})
        elseif target:is_plat("linux") then
            target:add("defines", "RADRAY_PLATFORM_UNIX", {public = true})
        elseif target:is_plat("macosx") then
            target:add("defines", "RADRAY_PLATFORM_UNIX", "RADRAY_PLATFORM_MACOS", {public = true})
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
