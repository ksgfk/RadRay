target("libpng")
    set_languages("c11")
    set_kind("static")
    add_rules("c.unity_build", {batchsize = 32})
    add_files("*.c" .. "|example.c|pngtest.c")
    if is_arch("x86", "x64", "i386", "x86_64") then
        add_files( "intel/*.c")
        add_defines("PNG_INTEL_SSE_OPT=1")
        add_vectorexts("sse4.2", "avx", "avx2")
    elseif is_arch("arm.*") then
        add_files( "arm/*.c")
        if is_plat("windows") then
            add_defines("PNG_ARM_NEON_OPT=1")
            add_defines("PNG_ARM_NEON_IMPLEMENTATION=1")
        else
            add_files( "arm/*.S")
            add_defines("PNG_ARM_NEON_OPT=2")
        end
        add_vectorexts("neon")
    elseif is_arch("mips.*") then
        add_files( "mips/*.c")
        add_defines("PNG_MIPS_MSA_OPT=2")
    elseif is_arch("ppc.*") then
        add_files( "powerpc/*.c")
        add_defines("PNG_POWERPC_VSX_OPT=2")
    elseif is_arch("loong64") then
        add_files( "loongarch/*.c")
        add_defines("PNG_LOONGARCH_LSX_OPT=1")
    end
    local incDir = path.absolute(path.join(os.projectdir(), "ext", "libpng_include", "include"))
    add_includedirs(incDir, {public = true})
    add_headerfiles( "*.h")
    add_deps("zlib")
    if is_plat("windows") then
        add_defines("_CRT_SECURE_NO_DEPRECATE", "_CRT_NONSTDC_NO_DEPRECATE")
    end
    on_load(function(target) 
        os.cp(path.join(os.scriptdir(), "scripts", "pnglibconf.h.prebuilt"), path.join(os.scriptdir(), "pnglibconf.h"))
    end)
target_end()
