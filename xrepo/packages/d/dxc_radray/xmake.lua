package("dxc_radray")
    set_urls("https://github.com/ksgfk/dxc_bin/releases/download/$(version)/dxc-$(version).zip")

    add_versions("v1.8.2407", "894a223491f0fee168c918f552aec35c2d067bfca2d2f28011d506b6e6af11b4")

    on_install(function (package)
        os.cp("linux-x64/include/*", package:installdir("include"))
        package:addenv("PATH", "include")
        if package:is_plat("windows") and package:is_arch("x64") then
            os.cp("win-x64/bin/*", package:installdir("bin"))
        end
        if package:is_plat("windows") and package:is_arch("arm64") then
            os.cp("win-arm64/bin/*", package:installdir("bin"))
        end
        if package:is_plat("linux") and package:is_arch("x64") then
            os.cp("linux-x64/lib/*", package:installdir())
        end
        if package:is_plat("macosx") and package:is_arch("x86_64") then
            os.cp("osx-x64/lib/*", package:installdir())
        end
        if package:is_plat("macosx") and package:is_arch("arm64") then
            os.cp("osx-arm64/lib/*", package:installdir())
        end
    end)
