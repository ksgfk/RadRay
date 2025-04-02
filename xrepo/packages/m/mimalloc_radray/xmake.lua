package("mimalloc_radray")
    set_homepage("https://github.com/microsoft/mimalloc")
    set_description("mimalloc (pronounced 'me-malloc') is a general purpose allocator with excellent performance characteristics.")
    set_license("MIT")

    set_urls("https://github.com/microsoft/mimalloc.git")

    add_versions("v2.1.7", "8c532c32c3c96e5ba1f2283e032f69ead8add00f")
    add_versions("v2.1.9", "a3070dc57fa03a43cae9079a2a879a66cec8f715")
    add_patches("v2.1.9", "patches/v2.1.9/0001-Merge-branch-dev-into-dev2.patch", "814650c4fa64ca33ca8f845d49fe88291c6762b9686be1c645d47afe6994e37e")
    add_versions("v2.2.2", "f81bf1b31af819a31195e08f9546dc80f8931587")
    add_versions("v2.2.3", "94036de6fe20bfd8a73d4a6d142fcf532ea604d9")

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
        local opts = {
            buildir = "build"
        }
        if package:is_plat("windows") and package:version_str() == "v2.2.2" then 
            -- workaround for v2.2.2, it cannot use ninja build on windows. but why?
            -- xmake v2.9.8, msbuild and clang-cl 同时在win上使用时
            -- xmake 会给 cmake 传递 CMAKE_GENERATOR_TOOLSET=v143, 和 clang-cl 的路径作为 CXX 编译器
            -- 但是 cmake 完全无视了编译器路径，依然使用 msvc
            -- what can I say man?
            opts.cmake_generator = "Visual Studio"
        end
        import("package.tools.cmake").build(package, configs, opts)
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
