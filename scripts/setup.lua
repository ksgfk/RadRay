option("_radray_checkout")
    set_default(false)
    set_showmenu(false)
    add_deps("build_test", "enable_d3d12")
    before_check(function(option)
        if path.absolute(path.join(os.projectdir(), "scripts")) == path.absolute(os.scriptdir()) then
            local opts = import("options", {try = true, anonymous = true})
            if opts then
                local opt = opts.get_options()
                for k, v in pairs(map) do
                    if v ~= nil then
                        option:dep(k):enable(v)
                    end
                end
            end
            local is_win = is_plat("windows")
            local enable_d3d12 = option:dep("enable_d3d12")
            if enable_d3d12:enabled() and not is_win then
                enable_d3d12:enable(false, {force = true})
                if enable_d3d12:enabled() then
                    error("d3d12 only support on windows")
                end
            end
        end
    end)
option_end()

rule("radray_basic_setting")
    on_load(function(target) 
        target:set("languages", "cxx20", "c17")
        target:set("warnings", "all")
        if is_mode("debug") then target:set("optimize", "none") else target:set("optimize", "aggressive") end
        if target:is_plat("windows") then
            target:add("defines", "RADRAY_PLATFORM_WINDOWS", {public = true})
        elseif target:is_plat("linux") then
            target:add("defines", "RADRAY_PLATFORM_LINUX", {public = true})
        end
        if is_mode("debug") then
            target:add("defines", "RADRAY_IS_DEBUG", {public = true})
        end
        target:add("cxflags", "/permissive-", "/utf-8", {tools = {"cl", "clang_cl"}})
        target:add("cxflags", "/Zc:preprocessor", "/Zc:__cplusplus", {tools = {"cl"}})
        target:add("vectorexts", "avx", "avx2", "neon")
        target:add("fpmodels", "fast")
    end)
rule_end()
