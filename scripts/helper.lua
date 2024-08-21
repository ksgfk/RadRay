function copy_if_newer(from, to) then

end

function copy_dxc_lib(target) 
    if get_config("enable_dxc") then
        local bin_dir = target:targetdir()
        if not os.isdir(bin_dir) then
            os.mkdir(bin_dir)
        end
        local dxc_dir = target:pkg("dxc_radray"):installdir()
        -- local bin_files = os.files(path.join(dxc_dir, "bin", "*.dll"))
        -- for _, file in ipairs(bin_files) do
        --     if path.extension(file) == ".dll" then
                
        --     end
        -- end

        os.cp(path.join(dxc_dir, "bin", "*.dll"), bin_dir)
        os.cp(path.join(dxc_dir, "lib", "*.so"), bin_dir)
        os.cp(path.join(dxc_dir, "lib", "*.dylib"), bin_dir)
    end
end
