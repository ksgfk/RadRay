includes("core")

if get_config("build_test") then
    includes("test")
end

if is_plat("windows") then
    includes("d3d12")
end
