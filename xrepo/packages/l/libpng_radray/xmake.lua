package("libpng_radray")
    set_base("libpng")

    on_install(function (package)
        io.writefile("xmake.lua", [[
            add_rules("mode.debug", "mode.release")
            add_requires("zlib")
            target("png")
                set_kind("$(kind)")
                add_files("*.c|example.c|pngtest.c")
                if is_arch("x86", "x64", "i386", "x86_64") then
                    add_files("intel/*.c")
                    add_defines("PNG_INTEL_SSE_OPT=1")
                    add_vectorexts("sse4.2", "avx", "avx2")
                elseif is_arch("arm.*") then
                    add_files("arm/*.c")
                    if is_plat("windows") then
                        add_defines("PNG_ARM_NEON_OPT=1")
                        add_defines("PNG_ARM_NEON_IMPLEMENTATION=1")
                    else
                        add_files("arm/*.S")
                        add_defines("PNG_ARM_NEON_OPT=2")
                    end
                    add_vectorexts("neon")
                elseif is_arch("mips.*") then
                    add_files("mips/*.c")
                    add_defines("PNG_MIPS_MSA_OPT=2")
                elseif is_arch("ppc.*") then
                    add_files("powerpc/*.c")
                    add_defines("PNG_POWERPC_VSX_OPT=2")
                elseif is_arch("loong64") then
                    add_files( "loongarch/*.c")
                    add_defines("PNG_LOONGARCH_LSX_OPT=1")
                end
                add_headerfiles("*.h")
                add_packages("zlib")
                if is_kind("shared") and is_plat("windows") then
                    add_defines("PNG_BUILD_DLL")
                end
        ]])
        local configs = {}
        if package:is_plat("android") and package:is_arch("armeabi-v7a") then
            io.replace("arm/filter_neon.S", ".func", ".hidden", {plain = true})
            io.replace("arm/filter_neon.S", ".endfunc", "", {plain = true})
        end
        os.cp("scripts/pnglibconf.h.prebuilt", "pnglibconf.h")
        import("package.tools.xmake").install(package, configs)
    end)
