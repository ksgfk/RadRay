target("spdlog")
    set_kind("static")
    add_rules("c++.unity_build", {batchsize = 32})
    if is_plat("linux", "bsd") then
        add_syslinks("pthread")
    end
    set_languages("cxx20")
    set_warnings("all")
    if is_mode("debug") then set_optimize("none") else set_optimize("aggressive") end
    add_defines("SPDLOG_COMPILED_LIB", "SPDLOG_DISABLE_DEFAULT_LOGGER", "SPDLOG_NO_EXCEPTIONS", "SPDLOG_NO_THREAD_ID", "SPDLOG_USE_STD_FORMAT", {public = true})
    add_includedirs("include", {public = true})
    add_headerfiles("include/**.h")
    add_files("src/*.cpp")
target_end()
