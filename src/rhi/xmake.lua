target("radray_rhi")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core")
    if get_config("enable_d3d12") then
        add_defines("RADRAY_ENABLE_D3D12", {public = true})
        add_deps("radray_d3d12")
    end
    if get_config("enable_metal") then
        add_defines("RADRAY_ENABLE_METAL", {public = true})
        add_deps("radray_metal")
    end
target_end()

if get_config("enable_d3d12") then
    includes("d3d12")
end

if get_config("enable_metal") then 
    includes("metal")
end
