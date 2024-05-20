target("nri")
    set_kind("static")
    set_languages("cxx20")
    if is_mode("debug") then set_optimize("none") else set_optimize("aggressive") end
    set_warnings("all")
    add_vectorexts("sse4.2", "avx", "avx2", "neon")
    add_defines("WIN32_LEAN_AND_MEAN", "NOMINMAX", "_CRT_SECURE_NO_WARNINGS", {tools = {"msvc", "clang_cl"}})
    add_includedirs("Include", {public = true})
    add_includedirs("Source/Shared")
    add_files("Source/Shared/*.cpp")
    add_files("Source/Creation/*.cpp")
    add_files("Source/Validation/*.cpp")
    if is_plat("windows") then
        add_files("Resources/*.rc")
    end
    local is_enable_d3d12 = get_config("enable_d3d12")
    if is_enable_d3d12 and is_plat("windows") then
        add_files("Source/D3D12/*.cpp")
        add_defines("NRI_USE_D3D12=1")
        add_syslinks("d3d12", "dxgi", "dxguid")
    end
    local is_enable_vulkan = get_config("enable_vulkan")
    if is_enable_vulkan then
        add_includedirs("External/vulkan/include")
        add_files("Source/VK/*.cpp")
        add_defines("NRI_USE_VULKAN=1")
        if is_plat("windows") then
            add_defines("VK_USE_PLATFORM_WIN32_KHR")
        elseif is_plat("macos") then
            add_defines("VK_USE_PLATFORM_METAL_EXT")
        else
            add_defines("VK_USE_PLATFORM_XLIB_KHR")
            add_deps("libx11")
        end
    end
    on_load(function(target)
        if target:is_plat("windows") then
            target:add("includedirs", path.join(os.scriptdir(), "External"))
            target:add("linkdirs", path.join(os.scriptdir(), "External/amdags/ags_lib/lib"))
            if is_arch("x64") then
                target:add("linkdirs", path.join(os.scriptdir(), "External/nvapi/amd64"))
                target:add("links", "nvapi64", "amd_ags_x64")
            elseif is_arch("x86") then
                target:add("linkdirs", path.join(os.scriptdir(), "External/nvapi/x86"))
                target:add("links", "nvapi", "amd_ags_x86")
            end
        end
    end)
    after_build(function(target)
        if is_plat("windows") then
            local bin_dir = target:targetdir()
            local dll_path = ""
            if is_arch("x64") then
                dll_path = "External/amdags/ags_lib/lib/amd_ags_x64.dll"
            elseif is_arch("x86") then
                dll_path = "External/amdags/ags_lib/lib/amd_ags_x86.dll"
            end
            os.cp(path.join(os.scriptdir(), dll_path), bin_dir)
        end
    end)
target_end()
