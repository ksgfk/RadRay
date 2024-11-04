package("directxshadercompiler_radray")
    set_base("directxshadercompiler")

    local date = {["v1.8.2407"] = "2024_07_31_clang_cl"}
    if is_plat("windows") then
        add_urls("https://github.com/microsoft/DirectXShaderCompiler/releases/download/v$(version).zip", {version = function (version) return version .. "/dxc_" .. date[tostring(version)] end})
        add_versions("v1.8.2407", "d6650a1b431fb24e47507b615c77f8a9717cd03e422ee12d4a1e98856f9ba7a6")
    elseif is_plat("linux") then
        add_urls("https://github.com/microsoft/DirectXShaderCompiler.git")
        add_versions("v1.8.2407", "416fab6b5c4ba956a320d9131102304da995edfc")

        add_extsources("pacman::directx-shader-compiler")
        add_deps("cmake", "ninja")
    elseif is_plat("macosx") then
        add_urls("https://github.com/microsoft/DirectXShaderCompiler.git")
        add_versions("v1.8.2407", "416fab6b5c4ba956a320d9131102304da995edfc")

        add_deps("cmake")
    end

    on_install("windows|arm64", function (package)
        os.cp("bin/arm64/*", package:installdir("bin"))
        os.cp("inc/*", package:installdir("include"))
        os.cp("lib/arm64/*", package:installdir("lib"))
        package:addenv("PATH", "bin")
    end)

    on_install("linux", function (package)
        if os.exists(package:name()) then -- todo: workaround
            os.cd(package:name())
        end
        if package:has_tool("cxx", "clang") then
            package:add("cxxflags", "-fms-extensions")
        end
        local configs = {
            format("-C ../%s/cmake/caches/PredefinedParams.cmake", package:name())
        }
        table.insert(configs, "-DCMAKE_BUILD_TYPE=Release")
        import("package.tools.cmake").build(package, configs, {cmake_generator = "Ninja", buildir = "../build"})
        os.cp("../build/bin/dxc", package:installdir("bin"))
        os.cp("include/dxc/", package:installdir("include"), {rootdir = "include/dxc"})
        os.cp("../build/lib/libdxcompiler.dylib*", package:installdir("lib"))
        package:addenv("PATH", "bin")
    end)

    on_install("macosx", function (package)
        if os.exists(package:name()) then -- todo: workaround
            os.cd(package:name())
        end
        local configs = {
            format("-C ../%s/cmake/caches/PredefinedParams.cmake", package:name())
        }
        table.insert(configs, "-DCMAKE_BUILD_TYPE=Release")
        import("package.tools.cmake").build(package, configs, {cmake_generator = "Ninja", buildir = "../build"})
        os.cp("../build/bin/dxc", package:installdir("bin"))
        os.cp("include/dxc/", package:installdir("include"), {rootdir = "include/dxc"})
        os.cp("../build/lib/libdxcompiler.dylib*", package:installdir("lib"))
        package:addenv("PATH", "bin")
    end)