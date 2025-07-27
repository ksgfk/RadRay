target("radray_render")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core")
    if get_config("enable_dxc") then
        add_defines("RADRAY_ENABLE_DXC", {public = true})
        add_packages("directxshadercompiler_radray")
    end
    if get_config("enable_spirv_cross") then
        add_defines("RADRAY_ENABLE_SPIRV_CROSS", {public = true})
        add_packages("spirv-cross_radray")
    end
    -- if get_config("enable_d3d12") then
    --     add_defines("RADRAY_ENABLE_D3D12", {public = true})
    --     add_files("d3d12/*.cpp")
    --     add_packages("directx-headers", "d3d12-memory-allocator")
    --     add_syslinks("d3d12", "dxgi", "dxguid")
    -- end
    if get_config("enable_metal") then
        add_defines("RADRAY_ENABLE_METAL", {public = true})
        add_files("metal/*.cpp")
        add_packages("metal-cpp")
        add_frameworks("Foundation", "Metal", "QuartzCore")
    end
    if get_config("enable_vulkan") then
        add_defines("RADRAY_ENABLE_VULKAN", {public = true})
        add_includedirs("vk/ext/Vulkan-Headers/include", {public = true})
        add_includedirs("vk/ext/volk/include", {public = true})
        add_includedirs("vk/ext/VulkanMemoryAllocator/include", {public = true})
        add_files("vk/*.cpp")
        add_files("vk/ext/volk/src/*.c")
        add_files("vk/ext/VulkanMemoryAllocator/src/*.cpp")
    end

    on_install(function (target)
        local helper = import("scripts.helper", {rootdir = os.projectdir()})
        helper.copy_dxil_dll(target)
    end)
