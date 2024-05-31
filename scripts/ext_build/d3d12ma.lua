target("d3d12ma")
    set_kind("static")
    set_languages("cxx20")
    set_warnings("all")
    if is_mode("debug") then set_optimize("none") else set_optimize("aggressive") end
    add_includedirs("include", {public = true})
    add_headerfiles("include/**.h")
    add_files("src/D3D12MemAlloc.cpp")
    add_ldflags("/NATVIS:" .. path.join(os.scriptdir(), "src", "D3D12MemAlloc.natvis") , {expand = false, tools = {"clang_cl", "cl"}})
target_end()
