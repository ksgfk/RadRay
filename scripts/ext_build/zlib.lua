target("zlib")
    set_languages("c11")
    set_kind("static")
    if not is_plat("windows") then
        set_basename("z")
    end
    add_files("adler32.c", "compress.c", "crc32.c", "deflate.c", "gzclose.c", "gzlib.c", "gzread.c", "gzwrite.c", "inflate.c", "infback.c", "inftrees.c", "inffast.c", "trees.c", "uncompr.c", "zutil.c")
    local incDir = path.absolute(path.join(os.projectdir(), "ext", "zlib_include", "include"))
    add_includedirs(incDir, {public = true})
    add_vectorexts("avx", "avx2", "neon")
    add_headerfiles("zlib.h", "zconf.h")
    includes("@builtin/check")
    check_cincludes("Z_HAVE_UNISTD_H", "unistd.h")
    check_cincludes("HAVE_SYS_TYPES_H", "sys/types.h")
    check_cincludes("HAVE_STDINT_H", "stdint.h")
    check_cincludes("HAVE_STDDEF_H", "stddef.h")
    if is_plat("windows") then
        add_defines("_CRT_SECURE_NO_DEPRECATE", "_CRT_NONSTDC_NO_DEPRECATE")
    else
        add_defines("ZEXPORT=__attribute__((visibility(\"default\"))", "_LARGEFILE64_SOURCE=1")
    end
target_end()
