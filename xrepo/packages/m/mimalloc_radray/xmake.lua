package("mimalloc_radray")
    set_homepage("https://github.com/microsoft/mimalloc")
    set_description("mimalloc (pronounced 'me-malloc') is a general purpose allocator with excellent performance characteristics.")
    set_license("MIT")

    set_urls("https://github.com/microsoft/mimalloc/archive/refs/tags/$(version).zip")

    add_versions("v2.1.7", "fa61cf01e3dd869b35275bfd8be95bfde77f0b65dfa7e34012c09a66e1ea463f")

    add_deps("cmake")

    if is_plat("windows") then
        add_syslinks("advapi32", "bcrypt")
    elseif is_plat("linux") then
        add_syslinks("pthread")
    elseif is_plat("android") then
        add_syslinks("atomic")
    end

    on_install(function (package)
        if package:is_plat("windows") and package:config("shared") then
            package:add("defines", "MI_SHARED_LIB")
        end
        if package:is_plat("wasm") then
            package:add("ldflags", "-sMALLOC=emmalloc")
        end
        local configs = {
            "-DMI_OVERRIDE=OFF",
            "-DMI_BUILD_TESTS=OFF",
            "-DMI_BUILD_OBJECT=OFF",
            "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"),
            "-DMI_DEBUG_FULL=" .. (package:is_debug() and "ON" or "OFF"),
            "-DMI_BUILD_STATIC=" .. (package:config("shared") and "OFF" or "ON"),
            "-DMI_BUILD_SHARED=" .. (package:config("shared") and "ON" or "OFF"),
            "-DMI_INSTALL_TOPLEVEL=ON"
        }
        import("package.tools.cmake").build(package, configs, {buildir = "build"})
        if package:is_plat("windows") then
            os.trycp("build/**.dll", package:installdir("bin"))
            os.trycp("build/**.lib", package:installdir("lib"))
        elseif package:is_plat("macosx") then
            os.trycp("build/*.dylib", package:installdir("lib"))
            os.trycp("build/*.a", package:installdir("lib"))               
        else
            os.trycp("build/*.so", package:installdir("lib"))
            os.trycp("build/*.a", package:installdir("lib"))
        end
        os.cp("include", package:installdir())
    end)

    on_test(function (package)
        assert(package:has_cfuncs("mi_malloc", {includes = "mimalloc.h"}))
    end)
