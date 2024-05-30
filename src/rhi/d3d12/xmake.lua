target("radray_d3d12")
    set_kind("static")
    add_rules("c++.unity_build", {batchsize = 32})
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core", "directx-headers")
    add_syslinks("dxgi", "d3d12")
target_end()
