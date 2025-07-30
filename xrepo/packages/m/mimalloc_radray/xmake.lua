package("mimalloc_radray")
    set_base("mimalloc")

    add_versions("v2.2.3", "94036de6fe20bfd8a73d4a6d142fcf532ea604d9")
    add_versions("v2.2.4", "fbd8b99c2b828428947d70fdc046bb55609be93e")

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
        import("package.tools.cmake").build(package, configs, {builddir = "build"})
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
