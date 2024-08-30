function copy_file_if_newer(from, to)
    if not os.isfile(from) then
        error(from .. "is not a file")
        return
    end
    if not os.isfile(to) then
        os.cp(from, to)
        print("copy", from, "to", to)
        return
    end
    if os.mtime(from) > os.mtime(to) then
        os.cp(from, to)
        print("copy", from, "to", to)
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
    for _, file in ipairs(os.files(path.join(dxc_dir, "lib", "*.so"))) do
        copy_file_if_newer(file, path.join(bin_dir, path.filename(file)))
    end
    for _, file in ipairs(os.files(path.join(dxc_dir, "lib", "*.dylib"))) do
        copy_file_if_newer(file, path.join(bin_dir, path.filename(file)))
    end
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
