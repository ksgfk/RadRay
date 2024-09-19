option("_radray_checkout")
    set_default(false)
    set_showmenu(false)
    add_deps("build_test", "enable_d3d12", "enable_metal", "enable_mimalloc", "enable_dxc", "enable_msc")
    before_check(function(option)
        if path.absolute(path.join(os.projectdir(), "scripts")) == path.absolute(os.scriptdir()) then
            local opts = import("options", {try = true, anonymous = true})
            if opts then
                local opt = opts.get_options()
                for k, v in pairs(opt) do
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
            local is_macos = is_plat("macosx")
            local enable_metal = option:dep("enable_metal")
            if enable_metal:enabled() and not is_macos then
                enable_metal:enable(false, {force = true})
                if enable_metal:enabled() then
                    error("metal only support on macosx")
                end
            end

            print("radray is enable d3d12", enable_d3d12:enabled())
            print("radray is enable metal", enable_metal:enabled())

            local enable_msc = option:dep("enable_msc")
            if not enable_metal:enabled() then
                enable_msc:enable(false, {force = true})
            end
        end
    end)
option_end()

rule("radray_basic_setting")
    on_load(function(target) 
        target:set("languages", "cxx20", "c17")
        target:set("warnings", "allextra")
        if is_mode("debug") then target:set("optimize", "none") else target:set("optimize", "aggressive") end
        if target:is_plat("windows") then
            target:add("defines", "RADRAY_PLATFORM_WINDOWS", {public = true})
        elseif target:is_plat("linux") then
            target:add("defines", "RADRAY_PLATFORM_LINUX", {public = true})
        elseif target:is_plat("macosx") then
            target:add("defines", "RADRAY_PLATFORM_MACOS", {public = true})
            target:add("mflags", "-fno-objc-arc")
        end
        if is_mode("debug") then
            target:add("defines", "RADRAY_IS_DEBUG", {public = true})
        end
        target:add("cxflags", "/permissive-", "/utf-8", {tools = {"cl", "clang_cl"}})
        target:add("cxflags", "/Zc:preprocessor", "/Zc:__cplusplus", {tools = {"cl"}})
        target:add("cxflags", "-stdlib=libc++", {tools = "clang"})
        target:add("vectorexts", "sse4.2", "avx", "avx2", "neon")
    end)
rule_end()
