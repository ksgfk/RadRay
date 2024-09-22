function copy_file_if_newer(from, to)
    if not os.isfile(from) then
        error(from .. "is not a file")
        return
    end
    if not os.isfile(to) then
        os.cp(from, to)
        print("copy", from, "->", to)
        return
    end
    if os.mtime(from) > os.mtime(to) then
        os.cp(from, to)
        print("copy", from, "->", to)
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

function copy_dxc_lib(target)
    local bin_dir = target:targetdir()
    if not os.isdir(bin_dir) then
        os.mkdir(bin_dir)
    end
    local dxc_dir = target:pkg("dxc_radray"):installdir()
    for _, file in ipairs(os.files(path.join(dxc_dir, "bin", "*.dll"))) do
        copy_file_if_newer(file, path.join(bin_dir, path.filename(file)))
    end
    for _, file in ipairs(os.files(path.join(dxc_dir, "*.so"))) do
        copy_file_if_newer(file, path.join(bin_dir, path.filename(file)))
    end
    for _, file in ipairs(os.files(path.join(dxc_dir, "*.dylib"))) do
        copy_file_if_newer(file, path.join(bin_dir, path.filename(file)))
    end
end

function copy_msc_lib(target)
    local bin_dir = target:targetdir()
    if not os.isdir(bin_dir) then
        os.mkdir(bin_dir)
    end
    local mcpp_dir = target:pkg("metalcpp"):installdir()
    for _, file in ipairs(os.files(path.join(mcpp_dir, "*.dylib"))) do
        copy_file_if_newer(file, path.join(bin_dir, path.filename(file)))
    end
end

function copy_shader_lib(rhi_target)
    local src_dir = path.join(rhi_target:scriptdir(), "shader_lib")
    local dst_dir = path.join(rhi_target:targetdir(), "shader_lib")
    copy_dir_if_newer_recursive(src_dir, dst_dir)
    clear_dst_dir_no_exist_file(src_dir, dst_dir)
end

function copy_example_shaders(target)
    local shader_src = path.join(target:scriptdir(), "shaders")
    local shader_dst = path.join(target:targetdir(), "shaders", target:name())
    copy_dir_if_newer_recursive(shader_src, shader_dst)
    clear_dst_dir_no_exist_file(shader_src, shader_dst)
end

function build_radray_rhi_swift(target, isConfig) 
    local mode = is_mode("debug") and "debug" or "release"
    local dir = path.join(os.projectdir(), "src", "rhi", "metal", "private")
    os.execv("swift", {"build", "-c", mode, "--package-path", dir})
    if isConfig then
        local libPath = os.iorunv("swift", {"build", "-c", mode, "--package-path", dir, "--show-bin-path"})
        local buildDir = string.trim(libPath)
        target:add("files", path.join(buildDir, "*.a"))
    end
end
