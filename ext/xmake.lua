target("eigen")
    local libDir = path.join(os.scriptdir(), "eigen")
    set_languages("cxx20")
    set_kind("headeronly")
    add_includedirs(libDir, {public = true})
    for _, filepath in ipairs(os.files(path.join(libDir, "Eigen", "**"))) do
        if os.isfile(filepath) then
            add_headerfiles(filepath, {prefixdir = path.join("include", path.directory(path.relative(filepath, libDir)))})
        end
    end
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")
target_end()

target("spdlog")
    local libDir = path.join(os.scriptdir(), "spdlog")
    set_languages("cxx20")
    set_kind("static")
    add_rules("radray_set_ucrt", "radray_no_rtti")
    add_includedirs(path.join(libDir, "include"), {public = true})
	add_defines("SPDLOG_NO_EXCEPTIONS", "SPDLOG_NO_THREAD_ID", "SPDLOG_DISABLE_DEFAULT_LOGGER", "FMT_SHARED", "FMT_CONSTEVAL=constexpr", "FMT_USE_CONSTEXPR=1", "FMT_EXCEPTIONS=0", {public = true})
	add_defines("FMT_EXPORT", "spdlog_EXPORTS", "SPDLOG_COMPILED_LIB")
    for _, filepath in ipairs(os.files(path.join(libDir, "include", "**"))) do
        if os.isfile(filepath) then
            add_headerfiles(filepath, {prefixdir = path.directory(path.relative(filepath, libDir))})
        end
    end
    add_files(path.join(libDir, "src", "*.cpp"))
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")
target_end()

target("glfw")
    local libDir = path.join(os.scriptdir(), "glfw")
    set_languages("cxx20", "c11")
    set_kind("static")
    add_rules("radray_set_ucrt")
    add_includedirs(path.join(libDir, "include"), {public = true})
    add_defines("_GLFW_BUILD_DLL")
    if is_plat("linux") then
        add_defines("_GLFW_X11", "_DEFAULT_SOURCE")
    elseif is_plat("windows") then
        add_defines("_GLFW_WIN32")
        add_syslinks("User32", "Gdi32", "Shell32")
    elseif is_plat("macosx") then
        add_files("../ext/glfw/src/*.m")
        add_mflags("-fno-objc-arc")
        add_defines("_GLFW_COCOA")
        add_frameworks("Foundation", "Cocoa", "IOKit", "OpenGL")
    end
    add_headerfiles(path.join(libDir, "include", "GLFW", "*.h"), {prefixdir = "include/GLFW"})
    add_files(path.join(libDir, "src", "*.c"))
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")
target_end()

target("xxhash")
    local libDir = path.join(os.scriptdir(), "xxHash")
    set_languages("cxx20", "c11")
    set_kind("static")
    add_rules("radray_set_ucrt")
    add_includedirs(libDir, {public = true})
    if is_arch("arm64") then
        add_vectorexts("neon")
    elseif is_arch("x64", "x86_64") then
        add_vectorexts("avx", "avx2")
    end
    add_headerfiles(path.join(libDir, "xxhash.h"), {prefixdir = "include"})
    add_files(path.join(libDir, "xxhash.c"))
    add_rules("utils.install.cmake_importfiles")
    add_rules("utils.install.pkgconfig_importfiles")
target_end()
