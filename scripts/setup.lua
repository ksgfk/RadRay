option("_radray_checkout")
    set_default(false)
    set_showmenu(false)
    add_deps("build_test", "enable_d3d12", "enable_metal", "enable_vulkan", "enable_mimalloc", "enable_dxc", "enable_spirv_cross", "enable_png")
    before_check(function(option)
        if path.absolute(path.join(os.projectdir(), "scripts")) == path.absolute(os.scriptdir()) then
            local opts = import("options", {try = true, anonymous = true})
            if opts then
                local opt = opts.get_options()
                for k, v in pairs(opt) do
                    if v ~= nil then
                        option:dep(k):enable(v)
                        print("set option:", k, v)
                    end
                end
            end
            local is_win = is_plat("windows")
            local enable_d3d12 = option:dep("enable_d3d12")
            if enable_d3d12:enabled() and not is_win then
                if enable_d3d12:enabled() then
                    print("d3d12 only support on windows")
                end
                enable_d3d12:enable(false, {force = true})
            end
            local is_macos = is_plat("macosx", "iphoneos")
            local enable_metal = option:dep("enable_metal")
            if enable_metal:enabled() and not is_macos then
                if enable_metal:enabled() then
                    print("metal only support on macosx or iphoneos")
                end
                enable_metal:enable(false, {force = true})
            end
            local enable_vulkan = option:dep("enable_vulkan")

            print("radray is enable d3d12", enable_d3d12:enabled())
            print("radray is enable metal", enable_metal:enabled())
            print("radray is enable vulkan", enable_vulkan:enabled())
            print("radray is enable mimalloc", option:dep("enable_mimalloc"):enabled())
            print("radray is enable dxc", option:dep("enable_dxc"):enabled())
            print("radray is enable spirv-cross", option:dep("enable_spirv_cross"):enabled())
            print("radray is enable libpng", option:dep("enable_png"):enabled())
        end
    end)
option_end()

rule("radray_basic_setting")
    on_load(function(target) 
        -- lang
        target:set("languages", "cxx20", "c17")
        -- env define
        if target:is_plat("windows") then
            target:add("defines", "RADRAY_PLATFORM_WINDOWS", {public = true})
        elseif target:is_plat("linux") then
            target:add("defines", "RADRAY_PLATFORM_LINUX", {public = true})
        elseif target:is_plat("macosx") then
            target:add("defines", "RADRAY_PLATFORM_MACOS", {public = true})
            target:add("mflags", "-fno-objc-arc")
        elseif target:is_plat("iphoneos") then
            target:add("defines", "RADRAY_PLATFORM_IOS", {public = true})
            target:add("mflags", "-fno-objc-arc")
        end
        if is_mode("debug") then
            target:add("defines", "RADRAY_IS_DEBUG", {public = true})
        end
        -- warning
        target:set("warnings", "allextra", "pedantic")
        -- optimize
        if is_mode("debug") then target:set("optimize", "none") else target:set("optimize", "aggressive") end
        -- exception
        target:set("exceptions", "cxx")
        -- rtti
        target:add("cxflags", "/GR-", {tools = {"clang_cl", "cl"}, public = true})
        target:add("cxflags", "-fno-rtti", "-fno-rtti-data", {tools = {"clang"}, public = true})
        target:add("cxflags", "-fno-rtti", {tools = {"gcc"}, public = true})
        -- msvc
        target:add("cxflags", "/permissive-", "/utf-8", {tools = {"cl", "clang_cl"}})
        target:add("cxflags", "/Zc:preprocessor", "/Zc:__cplusplus", {tools = {"cl"}})
        -- clang
        if is_plat("linux") then
            local _, cc = target:tool("cxx")
            if (cc == "clang" or cc == "clangxx") then
                target:add("cxflags", "-stdlib=libc++", {force = true})
                target:add("syslinks", "c++")
            end
        end
        -- simd
        target:add("vectorexts", "sse", "sse2", "sse3", "ssse3", "sse4.2", "avx", "avx2", "fma", "neon")
        -- fma
        if is_arch("x64", "x86_64") then target:add("cxflags", "-mfma", {tools = {"clang", "gcc"}}) end
        -- link
        -- if is_mode("release") then target:set("symbols", "debug") end
    end)
    on_config(function (target)
        if is_plat("windows") then
            local toolchain_settings = target:toolchain("msvc")
            if not toolchain_settings then
                toolchain_settings = target:toolchain("clang-cl")
            end
            if not toolchain_settings then
                toolchain_settings = target:toolchain("llvm")
            end
            if toolchain_settings then
                local sdk_version = toolchain_settings:runenvs().WindowsSDKVersion
                local legal_sdk = false
                if sdk_version then
                    local parts = {}
                    for part in sdk_version:gmatch("[^%.]+") do
                        table.insert(parts, part)
                    end
                    if #parts > 0 then
                        if tonumber(parts[1]) > 10 then
                            legal_sdk = true
                        elseif tonumber(parts[1]) == 10 then
                            if #parts > 2 then
                                if tonumber(parts[3]) >= 22000 then
                                    legal_sdk = true
                                end
                            end
                        end
                    end
                end
                if not legal_sdk then
                    os.raise(target:name(), ": windows SDK version is too low, please update to 10.0.22000.0 or higher", "detect windows SDK", sdk_version)
                end
            end
        end
    end)
rule_end()

rule("radray_app")
    on_config(function (target) 
        target:add("defines", format("RADRAY_APPNAME=\"%s\"", target:name()))
    end)
rule_end()

rule("radray_copy_shaders_to_bin")
    after_build(function (target) 
        local helper = import("scripts.helper", {rootdir = os.projectdir()})
        helper.copy_example_shaders(target)
    end)

    after_install(function (target) 
        local helper = import("scripts.helper", {rootdir = os.projectdir()})
        helper.copy_example_shaders(target, true)
    end)
rule_end()

rule("radray_test")
    on_config(function (target)
        target:set("kind", "binary")
        target:add("packages", "gtest")
    end)
rule_end()

rule("radray_bench")
    on_config(function (target)
        target:set("kind", "binary")
        target:add("packages", "benchmark")
    end)
rule_end()

-- https://github.com/xmake-io/xmake/blob/dev/xmake/toolchains/llvm/xmake.lua
toolchain("llvm-macos-brew")
    set_kind("standalone")
    set_homepage("https://llvm.org/")
    set_description("A collection of modular and reusable compiler and toolchain technologies")
    set_runtimes("c++_static", "c++_shared", "stdc++_static", "stdc++_shared")

    set_sdkdir("/usr/local/opt/llvm")

    set_toolset("cc",     "clang")
    set_toolset("cxx",    "clang", "clang++")
    set_toolset("mxx",    "clang", "clang++")
    set_toolset("mm",     "clang")
    set_toolset("cpp",    "clang -E")
    set_toolset("as",     "clang")
    set_toolset("ld",     "clang++", "clang")
    set_toolset("sh",     "clang++", "clang")
    set_toolset("ar",     "llvm-ar")
    set_toolset("strip",  "llvm-strip")
    set_toolset("ranlib", "llvm-ranlib")
    set_toolset("objcopy","llvm-objcopy")
    set_toolset("mrc",    "llvm-rc")

    on_load(function (toolchain)
        if toolchain:is_plat("macosx") then
            local xcode_dir     = get_config("xcode")
            local xcode_sdkver  = toolchain:config("xcode_sdkver")
            local xcode_sdkdir  = nil
            if xcode_dir and xcode_sdkver then
                xcode_sdkdir = xcode_dir .. "/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX" .. xcode_sdkver .. ".sdk"
                toolchain:add("cxflags", {"-isysroot", xcode_sdkdir})
                toolchain:add("mxflags", {"-isysroot", xcode_sdkdir})
                toolchain:add("ldflags", {"-isysroot", xcode_sdkdir})
                toolchain:add("shflags", {"-isysroot", xcode_sdkdir})
            else
                local macsdk = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
                if os.exists(macsdk) then
                    toolchain:add("cxflags", {"-isysroot", macsdk})
                    toolchain:add("mxflags", {"-isysroot", macsdk})
                    toolchain:add("ldflags", {"-isysroot", macsdk})
                    toolchain:add("shflags", {"-isysroot", macsdk})
                end
            end
            toolchain:add("mxflags", "-fobjc-arc")

			toolchain:add("ldflags", format("-L%s/lib/", toolchain:sdkdir()))
			toolchain:add("ldflags", format("-L%s/lib/c++/", toolchain:sdkdir()))
			toolchain:add("ldflags", format("-Wl,-rpath,%s/lib/c++", toolchain:sdkdir()))

            toolchain:add("shflags", format("-L%s/lib/", toolchain:sdkdir()))
			toolchain:add("shflags", format("-L%s/lib/c++/", toolchain:sdkdir()))
			toolchain:add("shflags", format("-Wl,-rpath,%s/lib/c++", toolchain:sdkdir()))
        end
    end)
toolchain_end()
