target("metal-cpp")
    set_kind("static")
    add_includedirs("include", {public = true})
    add_files("src/*.cpp")
    add_headerfiles("include/**")
target_end()