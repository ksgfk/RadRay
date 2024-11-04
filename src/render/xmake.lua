if get_config("enable_d3d12") then
    add_requires("directx-headers v1.614.1", {debug = is_mode("debug")})
    add_requires("d3d12-memory-allocator_radray v2.0.1", {debug = is_mode("debug")})
end
if get_config("enable_dxc") then
    add_requires("directxshadercompiler_radray v1.8.2407")
end
if get_config("enable_metal") then 
    add_requires("metal-cpp macOS15_iOS18", {debug = is_mode("debug")})
end

target("radray_render")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core")
    if get_config("enable_dxc") then
        add_defines("RADRAY_ENABLE_DXC", {public = true})
        add_packages("directxshadercompiler_radray")
    end
    if get_config("enable_d3d12") then
        add_defines("RADRAY_ENABLE_D3D12", {public = true})
        add_files("d3d12/*.cpp")
        add_packages("directx-headers", "d3d12-memory-allocator_radray")
        add_syslinks("d3d12", "dxgi", "dxguid")
    end
    if get_config("enable_metal") then
        add_defines("RADRAY_ENABLE_METAL", {public = true})
        add_files("metal/*.cpp")
        add_packages("metal-cpp")
        add_frameworks("Foundation", "Metal", "QuartzCore")
    end
