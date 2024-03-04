package("ext_eigen")
    add_versions("3.4.0", "")
    set_sourcedir(path.join(os.scriptdir(), "eigen"))
    add_deps("cmake")
    add_includedirs("include/eigen3")
    on_install(function(package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        table.insert(configs, "-DBUILD_TESTING=OFF")
        import("package.tools.cmake").install(package, configs)
    end)
package_end()

package("ext_spdlog")
    add_versions("1.13.0", "")
    set_sourcedir(path.join(os.scriptdir(), "spdlog"))
    add_deps("cmake")
    on_install(function(package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        table.insert(configs, "-DSPDLOG_BUILD_SHARED=OFF")
        table.insert(configs, "-DSPDLOG_DISABLE_DEFAULT_LOGGER=ON")
        table.insert(configs, "-DSPDLOG_NO_THREAD_ID=ON")
        import("package.tools.cmake").install(package, configs)
    end)
package_end()

package("ext_d3d12ma")
    add_versions("2.0.1", "")
    set_sourcedir(path.join(os.scriptdir(), "D3D12MemoryAllocator"))
    add_deps("cmake")
    on_install(function(package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        import("package.tools.cmake").install(package, configs)
    end)
package_end()

package("ext_xxhash")
    add_versions("0.8.2", "")
    local dir = path.join(os.scriptdir(), "xxHash")
    set_sourcedir(dir)
    on_install(function (package)
        local function rm_cache()
            if os.exists(path.join(dir, "build")) then
               os.rmdir(path.join(dir, "build")) 
            end
            if os.exists(path.join(dir, ".xmake")) then
               os.rmdir(path.join(dir, ".xmake")) 
            end
            if os.exists(path.join(dir, "xmake.lua")) then
                os.rm(path.join(dir, "xmake.lua"))
            end
        end
        rm_cache()
        io.writefile("xmake.lua", [[
            add_rules("mode.debug", "mode.release")
            target("xxhash")
                set_kind("static")
                add_files("xxhash.c")
                add_headerfiles("xxhash.h", "xxh3.h")
                if is_arch("arm64") then
                    add_vectorexts("neon")
                elseif is_arch("x64", "x86_64") then
                    add_vectorexts("avx", "avx2")
                end
        ]])
        import("package.tools.xmake").install(package)
        rm_cache()
    end)
    on_test(function (package)
        assert(package:has_cfuncs("XXH_versionNumber", {includes = "xxhash.h"}))
    end)
package_end()

package("ext_glfw")
    add_versions("3.4.0", "")
    set_sourcedir(path.join(os.scriptdir(), "glfw"))
    if is_plat("macosx") then
        add_frameworks("Cocoa", "IOKit")
    elseif is_plat("windows") then
        add_syslinks("user32", "shell32", "gdi32")
    elseif is_plat("linux") then
        add_deps("libx11", "libxrandr", "libxrender", "libxinerama", "libxfixes", "libxcursor", "libxi", "libxext")
        add_syslinks("dl", "pthread")
        add_defines("_GLFW_X11")
    end
    add_deps("cmake")
    on_install(function(package)
        local configs = {}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        table.insert(configs, "-DGLFW_BUILD_EXAMPLES=OFF")
        table.insert(configs, "-DGLFW_BUILD_TESTS=OFF")
        table.insert(configs, "-DBUILD_SHARED_LIBS=OFF")
        import("package.tools.cmake").install(package, configs)
    end)
package_end()
