function copy_file_if_newer(from, to)
    if not os.isfile(from) then
        print(from, "is not a file")
        return
    end
    if not os.isfile(to) then
        os.cp(from, to)
        print("copy no exist", from, "->", to)
        return
    end
    if os.mtime(from) > os.mtime(to) then
        os.cp(from, to)
        print("copy newer", from, "->", to)
    end
end

function copy_dir_if_newer_recursive(from, to)
    for _, file in ipairs(os.files(path.join(from, "**"))) do
        local rela = path.relative(file, from)
        local dst = path.join(to, rela)
        copy_file_if_newer(file, dst)
    end
end

function clear_dst_dir_no_exist_file(from, to)
    for _, file in ipairs(os.files(path.join(to, "**"))) do
        local rela = path.relative(file, to)
        local src = path.join(from, rela)
        if not os.exists(src) then
            os.rm(file)
            print("rm not exist file", file)
        end
    end
end

function copy_app_data(target, is_install, from, to)
    local src_dir = path.join(target:scriptdir(), from)
    local dst_dir
    if is_install then
        dst_dir = path.join(target:installdir(), "bin", to and to or from)
    else
        dst_dir = path.join(target:targetdir(), to and to or from)
    end
    copy_dir_if_newer_recursive(src_dir, dst_dir)
    clear_dst_dir_no_exist_file(src_dir, dst_dir)
end

function copy_example_shaders(target, is_install)
    copy_app_data(target, is_install, "shaders", path.join("shaders", target:name()))
end

function copy_dxil_dll(target)
    if target:is_plat("windows") then
        local dxc = target:pkg("directxshadercompiler_radray")
        if (dxc) then
            local dxil = path.join(dxc:installdir(), "bin", "dxil.dll")
            if not os.exists(dxil) then 
                raise("dxil.dll not found, please check the installation of directxshadercompiler_radray")
            end
            local bin_dir = path.join(target:installdir(), "bin", "dxil.dll")
            copy_file_if_newer(dxil, bin_dir)
        end
    end
end
