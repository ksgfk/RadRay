package("metal-cpp")
    add_urls("https://developer.apple.com/metal/cpp/files/metal-cpp_$(version).zip")

    add_versions("macOS14.2_iOS17.2", "d800ddbc3fccabce3a513f975eeafd4057e07a29e905ad5aaef8c1f4e12d9ada")

    on_install("macosx", "iphoneos", function (package)
        os.execv("python", {"SingleHeader/MakeSingleHeader.py", "Foundation/Foundation.hpp", "QuartzCore/QuartzCore.hpp", "Metal/Metal.hpp", "MetalFX/MetalFX.hpp"})
        os.cp(path.join("SingleHeader", "Metal.hpp"), "include/Metal/")
        io.writefile("impl.cpp", [[
            #define NS_PRIVATE_IMPLEMENTATION
            #define MTL_PRIVATE_IMPLEMENTATION
            #define CA_PRIVATE_IMPLEMENTATION
            #include <Metal/Metal.hpp>
        ]])
        io.writefile("xmake.lua", [[
            target("metal-cpp")
                set_kind("$(kind)")
                set_languages("cxx17")
                add_includedirs("include", {public = true})
                add_files("impl.cpp")
                add_headerfiles("include/**", {prefixdir = "Metal"})
                add_frameworks("Foundation", "Metal", "QuartzCore")
        ]])
        import("package.tools.xmake").install(package)
    end)

    on_test(function (package)
        assert(package:has_cxxincludes("Metal/Metal.hpp"))
    end)
