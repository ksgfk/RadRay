package("metal-shaderconverter")
    add_versions("1.1", "dummy")

    add_configs("metal_cpp_version", {description = "metal-cpp version", type = "string"})

    add_includedirs("include", {public = true})

    on_load(function (package) 
        package:add("deps", format("metal-cpp %s", package:config("metal_cpp_version")), {debug = package:is_debug()})
    end)

    on_install("macosx", function (package)
        os.cp("/usr/local/include/metal_irconverter/*", "include/metal_irconverter/")
        os.cp("/usr/local/include/metal_irconverter_runtime/*", "include/metal_irconverter_runtime/")
        os.cp("/usr/local/lib/libmetalirconverter.dylib", package:installdir())
        local metalcpp = package:dep("metal-cpp")
        io.writefile("impl.cpp", [[
            #include <Metal/Metal.hpp>
            #define IR_RUNTIME_METALCPP
            #define IR_PRIVATE_IMPLEMENTATION
            #include <metal_irconverter_runtime/metal_irconverter_runtime.h>
            #include <metal_irconverter/metal_irconverter.h>
        ]])
        io.writefile("xmake.lua", format([[
            target("metal-shaderconverter")
                set_kind("$(kind)")
                set_languages("cxx17")
                add_includedirs("include", {public = true})
                add_includedirs("%s")
                add_files("impl.cpp")
                add_headerfiles("include/metal_irconverter/**", {prefixdir = "metal_irconverter"})
                add_headerfiles("include/metal_irconverter_runtime/**", {prefixdir = "metal_irconverter_runtime"})
                add_packages("metal-cpp", {links = {}})
        ]], metalcpp:installdir("include"))) -- hack, 为什么找不到metal-cpp include
        import("package.tools.xmake").install(package)
    end)

    on_test(function (package)
        assert(package:check_cxxsnippets({test = [[
            #include <Metal/Metal.hpp>
            #define IR_RUNTIME_METALCPP
            #include <metal_irconverter_runtime/metal_irconverter_runtime.h>
            void test(int argc, char** argv) {}
        ]]}, {configs = {languages = "cxx17"}}))
    end)
