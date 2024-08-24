if get_config("enable_dxc") then 
    add_requires("dxc_radray v1.8.2407", {debug = is_mode("debug")})
end

includes("core")
includes("window")
includes("rhi")
if get_config("build_test") then
    includes("test")
end
