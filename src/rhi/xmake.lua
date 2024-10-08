if get_config("enable_d3d12") then
    add_requires("directx-headers v1.614.1", {debug = is_mode("debug")})
    add_requires("d3d12-memory-allocator_radray v2.0.1", {
        alias = "d3d12-memory-allocator",
        debug = is_mode("debug")
    })
end
local _metalcpp_ver = "macOS14.2_iOS17.2"
if get_config("enable_metal") then 
    add_requires(format("metal-cpp %s", _metalcpp_ver), {debug = is_mode("debug")})
end
if get_config("enable_shader_compiler") then
    add_requires("dxc_radray v1.8.2407", {debug = is_mode("debug")})
    add_requires("spirv-cross_radray 1.3.290", {debug = is_mode("debug")})
    if is_plat("macosx") then
        add_requires("metal-shaderconverter", {debug = is_mode("debug"), configs = {dep_ver = _metalcpp_ver}})
    end
end

if get_config("enable_shader_compiler") then
    target("radray_shader_compiler")
        set_kind("shared")
        add_rules("radray_basic_setting")
        add_includedirs(path.join(os.projectdir(), "include"))
        add_files("shader_compiler/*.cpp")
        add_packages("dxc_radray", "spirv-cross_radray")
        if is_plat("windows") then 
            add_defines("RADRAYSC_ENABLE_DXC_CREATE_D3D12_REFL")
        end
        if is_plat("macosx") then
            add_packages("metal-shaderconverter", {links = {"metal-shaderconverter", "metalirconverter"}})
            add_defines("RADRAYSC_ENABLE_MSC")
        end
        after_build(function (target)
            local helper = import("scripts.helper", {rootdir = os.projectdir()})
            local dxc_dir = target:pkg("dxc_radray"):installdir()
            local tar_dir = target:targetdir()
            if is_plat("windows") then
                helper.copy_file_if_newer(path.join(dxc_dir, "bin", "dxcompiler.dll"), path.join(tar_dir, "dxcompiler.dll"))
                helper.copy_file_if_newer(path.join(dxc_dir, "bin", "dxil.dll"), path.join(tar_dir, "dxil.dll"))
            elseif is_plat("macosx") then 
                helper.copy_file_if_newer(path.join(dxc_dir, "lib", "libdxcompiler.dylib"), path.join(tar_dir, "lib", "libdxcompiler.dylib"))
                local msc_dir = target:pkg("metal-shaderconverter"):installdir()
                helper.copy_file_if_newer(path.join(msc_dir, "lib", "libmetalirconverter.dylib"), path.join(tar_dir, "lib", "libmetalirconverter.dylib"))
            else 
                helper.copy_file_if_newer(path.join(dxc_dir, "lib", "libdxcompiler.so"), path.join(tar_dir, "libdxcompiler.so"))
            end
        end)

        after_install(function (target) 
            local helper = import("scripts.helper", {rootdir = os.projectdir()})
            local dxc_dir = target:pkg("dxc_radray"):installdir()
            local inst_dir = target:installdir()
            if is_plat("windows") then
                helper.copy_file_if_newer(path.join(dxc_dir, "bin", "dxil.dll"), path.join(inst_dir, "bin", "dxil.dll"))
            end
        end)
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
        add_packages("metal-cpp")
    end

    on_config(function (target) 
        local tar_dir = target:targetdir()
        if get_config("enable_shader_compiler") then
            if not target:is_plat("windows") then
                local lib_path = path.join(os.projectdir(), tar_dir, "lib")
                do
                    local pkg_env = target._PKGENVS.LD_LIBRARY_PATH
                    pkg_env = format("%s:%s", pkg_env, lib_path)
                    target._PKGENVS.LD_LIBRARY_PATH = pkg_env
                end
                if is_plat("macosx") then
                    local pkg_env = target._PKGENVS.DYLD_LIBRARY_PATH
                    pkg_env = format("%s:%s", pkg_env, lib_path)
                    target._PKGENVS.DYLD_LIBRARY_PATH = pkg_env
                end
            end
        end
    end)

    after_build(function (target)
        local helper = import("scripts.helper", {rootdir = os.projectdir()})
        helper.copy_shader_lib(target)
    end)

    after_install(function (target)
        local helper = import("scripts.helper", {rootdir = os.projectdir()})
        helper.copy_shader_lib(target, true)
    end)
target_end()
