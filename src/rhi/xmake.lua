target("radray_rhi")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core")
    if get_config("enable_d3d12") then
        add_defines("RADRAY_ENABLE_D3D12")
        add_deps("radray_d3d12")
    end
target_end()

if get_config("enable_d3d12") then
    includes("d3d12")
end
