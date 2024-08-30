add_requires("dxc_radray v1.8.2407", {debug = is_mode("debug")})
if get_config("enable_d3d12") then
    add_requires("directx-headers v1.614.0", {debug = is_mode("debug")})
    add_requires("d3d12-memory-allocator_radray v2.0.1", {
        alias = "d3d12-memory-allocator",
        debug = is_mode("debug")
    })
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
    if get_config("enable_dxc") then 
        add_defines("RADRAY_ENABLE_DXC", {public = true})
    end
    add_packages("dxc_radray")

    on_config(function (target) 
        if get_config("enable_metal") then 
            import("scripts.helper", {rootdir = os.projectdir()}).build_radray_rhi_swift(target, true)
            target:add("defines", "RADRAY_ENABLE_METAL", {public = true})
            target:add("files", path.join(os.scriptdir(), "metal", "*.cpp"))
            target:add("frameworks", "Foundation", "Metal", "QuartzCore", "AppKit")
        end 
    end)

    before_build(function (target)
        if get_config("enable_dxc") then 
            import("scripts.helper", {rootdir = os.projectdir()}).copy_dxc_lib(target)
        end
        if get_config("enable_metal") then 
            import("scripts.helper", {rootdir = os.projectdir()}).build_radray_rhi_swift(target, false)
        end 
    end)

    before_install(function (target)
        if get_config("enable_dxc") then 
            import("scripts.helper", {rootdir = os.projectdir()}).copy_dxc_lib(target)
        end 
    end)
target_end()
