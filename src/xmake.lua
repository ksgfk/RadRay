includes("core")
includes("window")

if is_plat("windows") then
    includes("d3d12")
end

if get_config("build_test") then
    includes("test")
end
