if get_config("enable_d3d12") then
    add_requires("directx-headers v1.614.1", {debug = is_mode("debug")})
    add_requires("d3d12-memory-allocator_radray v2.0.1", {
        alias = "d3d12-memory-allocator",
        debug = is_mode("debug")
    })
end
if get_config("enable_metal") then 
    add_requires("metalcpp macOS14.2_iOS17.2", {debug = is_mode("debug")})
end
if get_config("enable_shader_compiler") then
    add_requires("dxc_radray v1.8.2407", {debug = is_mode("debug")})
    add_requires("spirv-cross_radray 1.3.290", {debug = is_mode("debug")})
end

if get_config("enable_shader_compiler") then
    target("radray_shader_compiler")
        set_kind("shared")
        add_rules("radray_basic_setting")
        add_files("shader_compiler/*.cpp")
        add_packages("dxc_radray", "spirv-cross_radray")
    target_end()
end

target("radray_rhi")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core")
    if get_config("enable_d3d12") then
        add_defines("RADRAY_ENABLE_D3D12", {public = true})
        add_files("d3d12/*.cpp")
        add_packages("directx-headers", "d3d12-memory-allocator")
        add_syslinks("d3d12", "dxgi", "dxguid")
    end
    if get_config("enable_metal") then 
        add_defines("RADRAY_ENABLE_METAL", {public = true})
        add_files("metal/*.cpp")
        add_files("metal/*.mm")
        add_frameworks("Foundation", "Metal", "QuartzCore", "AppKit")
        add_packages("metalcpp")
    end

    before_build(function (target)
        local helper = import("scripts.helper", {rootdir = os.projectdir()})
        helper.copy_shader_lib(target)
    end)

    before_install(function (target)
        local helper = import("scripts.helper", {rootdir = os.projectdir()})
        helper.copy_shader_lib(target)
    end)
target_end()
