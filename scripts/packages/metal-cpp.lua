package("metal-cpp")
    add_urls("https://developer.apple.com/metal/cpp/files/metal-cpp_$(version).zip")

    add_versions("macOS14.2_iOS17.2", "d800ddbc3fccabce3a513f975eeafd4057e07a29e905ad5aaef8c1f4e12d9ada")

    on_install("macosx", "iphoneos", function (package)
        os.cp(path.join("Foundation", "*"), "include/Foundation/")
        os.cp(path.join("Metal", "*"), "include/Metal/")
        os.cp(path.join("MetalFX", "*"), "include/MetalFX/")
        os.cp(path.join("QuartzCore", "*"), "include/QuartzCore/")
        io.writefile("impl.cpp", [[
            #define NS_PRIVATE_IMPLEMENTATION
            #define MTL_PRIVATE_IMPLEMENTATION
            #define CA_PRIVATE_IMPLEMENTATION
            #include <Foundation/Foundation.hpp>
            #include <Metal/Metal.hpp>
            #include <MetalFX/MetalFX.hpp>
            #include <QuartzCore/QuartzCore.hpp>
        ]])
        io.writefile("xmake.lua", [[
            target("metal-cpp")
                set_kind("$(kind)")
                set_languages("cxx17")
                add_includedirs("include", {public = true})
                add_files("impl.cpp")
                add_headerfiles("include/Foundation/**", {prefixdir = "Foundation"})
                add_headerfiles("include/Metal/**", {prefixdir = "Metal"})
                add_headerfiles("include/MetalFX/**", {prefixdir = "MetalFX"})
                add_headerfiles("include/QuartzCore/**", {prefixdir = "QuartzCore"})
                add_frameworks("Foundation", "Metal", "QuartzCore")
        ]])
        import("package.tools.xmake").install(package)
    end)

    on_test(function (package)
        assert(package:has_cxxincludes("Metal/Metal.hpp"))
    end)
