package("directxshadercompiler_radray")
    set_homepage("https://github.com/microsoft/DirectXShaderCompiler/")
    set_description("DirectX Shader Compiler")
    set_license("LLVM")
    if is_plat("windows") then
        add_urls("https://github.com/ksgfk/dxc-autobuild/releases/download/$(version)/dxc-windows-x64.zip")
        add_versions("v1.8.2505.1", "97bf7e1d6fef09f90696dd37877540906c29d04be6263d7d2cd1429d56801c1b")
    elseif is_plat("linux") then
        add_urls("https://github.com/ksgfk/dxc-autobuild/releases/download/$(version)/dxc-linux-x64.tar.gz")
        add_versions("v1.8.2505.1", "5840824ec40e16cb83da8aa2057a5240ce59cbb2d6d84c580dabafc8d0efe534")
    elseif is_plat("macosx") then
        add_urls("https://github.com/ksgfk/dxc-autobuild/releases/download/$(version)/dxc-macos-arm64.tar.gz")
        add_versions("v1.8.2505.1", "5d14598aeacc9c594a33bf91e2c668231e3572c72fc32021bb4c4cd399088920")
    end
    add_configs("shared", {description = "Using shared binaries.", default = true, type = "boolean", readonly = true})
    add_links("dxcompiler", "dxil")

    on_install("windows|x64", function (package)
        os.cp("bin/*", package:installdir("bin"))
        os.cp("include/*", package:installdir("include"))
        os.cp("lib/*", package:installdir("lib"))
        package:addenv("PATH", "bin")
    end)

    on_install("linux|x64", "macosx|arm64", function (package)
        os.cp("include/*", package:installdir("include"))
        os.cp("lib/*", package:installdir("lib"))
        package:addenv("PATH", "bin")
    end)
