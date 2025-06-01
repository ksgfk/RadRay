package("directxshadercompiler_radray")
    set_base("directxshadercompiler")

    local date = {
        ["v1.8.2407"] = "2024_07_31_clang_cl",
        ["v1.8.2502"] = "2025_02_20",
        ["v1.8.2505"] = "2025_05_24",
    }
    if is_plat("windows") then
        add_urls("https://github.com/microsoft/DirectXShaderCompiler/releases/download/$(version).zip", {version = function (version) return version .. "/dxc_" .. date[tostring(version)] end})
        add_versions("v1.8.2407", "d6650a1b431fb24e47507b615c77f8a9717cd03e422ee12d4a1e98856f9ba7a6")
        add_versions("v1.8.2502", "70b1913a1bfce4a3e1a5311d16246f4ecdf3a3e613abec8aa529e57668426f85")
        add_versions("v1.8.2505", "81380f3eca156d902d6404fd6df9f4b0886f576ff3e18b2cc10d3075ffc9d119")
    elseif is_plat("linux") then
        add_urls("https://github.com/microsoft/DirectXShaderCompiler.git")
        add_versions("v1.8.2407", "416fab6b5c4ba956a320d9131102304da995edfc")
        add_versions("v1.8.2502", "b4711839eb9a87da7c3436d9b212e0492359fbbd")
        add_versions("v1.8.2505", "9efbb6c3242cbb40c1844a2589171ff1c27cf956")

        add_extsources("pacman::directx-shader-compiler")
        add_deps("cmake", "ninja")
    elseif is_plat("macosx") then
        add_urls("https://github.com/microsoft/DirectXShaderCompiler.git")
        add_versions("v1.8.2407", "416fab6b5c4ba956a320d9131102304da995edfc")
        add_versions("v1.8.2502", "b4711839eb9a87da7c3436d9b212e0492359fbbd")
        add_versions("v1.8.2505", "9efbb6c3242cbb40c1844a2589171ff1c27cf956")

        add_deps("cmake")
    end

    on_install("windows|x64", function (package)
        os.cp("bin/x64/*", package:installdir("bin"))
        os.cp("inc/*", package:installdir("include"))
        os.cp("lib/x64/*", package:installdir("lib"))
        package:addenv("PATH", "bin")
    end)

    on_install("windows|arm64", function (package)
        os.cp("bin/arm64/*", package:installdir("bin"))
        os.cp("inc/*", package:installdir("include"))
        os.cp("lib/arm64/*", package:installdir("lib"))
        package:addenv("PATH", "bin")
    end)

    -- TODO: fix linux and macosx
    -- on_install("linux", function (package)
    --     if os.exists(package:name()) then -- todo: workaround
    --         os.cd(package:name())
    --     end
    --     if package:has_tool("cxx", "clang") then
    --         package:add("cxxflags", "-fms-extensions")
    --     end
    --     local configs = {
    --         format("-C ../%s/cmake/caches/PredefinedParams.cmake", package:name())
    --     }
    --     table.insert(configs, "-DCMAKE_BUILD_TYPE=Release")
    --     import("package.tools.cmake").build(package, configs, {cmake_generator = "Ninja", buildir = "../build"})
    --     os.cp("../build/bin/dxc", package:installdir("bin"))
    --     os.cp("include/dxc/", package:installdir("include"), {rootdir = "include/dxc"})
    --     os.cp("../build/lib/libdxcompiler.dylib*", package:installdir("lib"))
    --     package:addenv("PATH", "bin")
    -- end)

    -- on_install("macosx", function (package)
    --     if os.exists(package:name()) then -- todo: workaround
    --         os.cd(package:name())
    --     end
    --     local configs = {
    --         format("-C ../%s/cmake/caches/PredefinedParams.cmake", package:name())
    --     }
    --     table.insert(configs, "-DCMAKE_BUILD_TYPE=Release")
    --     import("package.tools.cmake").build(package, configs, {cmake_generator = "Ninja", buildir = "../build"})
    --     os.cp("../build/bin/dxc", package:installdir("bin"))
    --     os.cp("include/dxc/", package:installdir("include"), {rootdir = "include/dxc"})
    --     os.cp("../build/lib/libdxcompiler.dylib*", package:installdir("lib"))
    --     package:addenv("PATH", "bin")
    -- end)
