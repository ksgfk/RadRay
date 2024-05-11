target("glfw")
    set_kind("static")
    if is_plat("macosx") then
        add_frameworks("Cocoa", "IOKit")
        add_defines("_GLFW_COCOA")
    elseif is_plat("windows") then
        add_syslinks("user32", "shell32", "gdi32")
        add_defines("_GLFW_WIN32")
        add_defines("_CRT_SECURE_NO_WARNINGS")
    elseif is_plat("mingw") then
        add_syslinks("gdi32")
        add_defines("_GLFW_WIN32")
    elseif is_plat("linux") then
        add_syslinks("dl", "pthread")
        add_defines("_GLFW_X11")
        add_deps("libx11", "libxrandr", "libxrender", "libxinerama", "libxfixes", "libxcursor", "libxi", "libxext")
    end
    add_headerfiles("include/**.h")
    add_files("src/*.c")
    add_includedirs("include", {public = true})
target_end()
