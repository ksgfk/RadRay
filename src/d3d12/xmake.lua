target("radray_d3d12")
    add_rules("radray_basic_setting")
    set_kind("static")
    add_includedirs(path.join(os.projectdir(), "ext", "DirectXTK12"), {public = true})
    add_files("*.cpp")
    add_deps("radray_core")
    add_syslinks("dxgi", "d3d12")
    on_load(function(target) 
        local lib_dir = path.join(os.projectdir(), "ext", "dxc", "lib")
        local inc_dir = path.join(os.projectdir(), "ext", "dxc", "inc")
        if target:is_arch("x86") then
            target:add("linkdirs", path.join(lib_dir, "x86"))
        elseif target:is_arch("x64", "x86_64") then
            target:add("linkdirs", path.join(lib_dir, "x64"))
        elseif target:is_arch("arm64") then
            target:add("linkdirs", path.join(lib_dir, "arm64"))
        end
        target:add("links", "dxcompiler")
        target:add("includedirs", inc_dir, {public = true})
    end)
    after_build(function(target)
        local bin_dir = target:targetdir()
        local dll_dir = path.join(os.projectdir(), "ext", "dxc", "bin")
        if target:is_arch("x86") then
            os.cp(path.join(dll_dir, "x86", "*.dll"), bin_dir)
        elseif target:is_arch("x64", "x86_64") then
            os.cp(path.join(dll_dir, "x64", "*.dll"), bin_dir)
        elseif target:is_arch("arm64") then
            os.cp(path.join(dll_dir, "arm64", "*.dll"), bin_dir)
        end
    end)
target_end()
