package("dxc_radray")
    set_urls("https://github.com/ksgfk/dxc_bin/releases/download/$(version)/dxc-$(version).zip")

    add_versions("v1.8.2407", "e02b9d47da0299979e7b939c50c77ef45774f6302543c35e2f8a24557c15d9cd")

    on_install("linux|x64", function (package)
        os.cp("linux-x64/*", package:installdir())
        package:addenv("PATH", "bin")
    end)

    on_install("macosx|x86_64", function (package) 
        os.cp("osx-x64/*", package:installdir())
        package:addenv("PATH", "bin")
    end)

    on_install("macosx|arm64", function (package) 
        os.cp("osx-arm64/*", package:installdir())
        package:addenv("PATH", "bin")
    end)

    on_install("windows|x64", function (package) 
        os.cp("win-x64/*", package:installdir())
        package:addenv("PATH", "bin")
        package:add("includedirs", "inc")
    end)

    on_install("windows|arm64", function (package) 
        os.cp("win-arm64/*", package:installdir())
        package:addenv("PATH", "bin")
        package:add("includedirs", "inc")
    end)
