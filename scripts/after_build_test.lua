import("core.project.config")
import("core.project.project")
local options = import("options", {try = true, anonymous = true})
local helper = import("helper", {try = true, anonymous = true})

function main(args)
    config.load()

    local conf = options.get_test_assets_config()
    local src = path.join(path.absolute(os.projectdir()), conf.src_path)
    print("copy from", src)

    local target = project.target("radray_core")
    if not target then
        print("please config project first, run `xmake f -m debug/release -v`")
        return -1
    end

    local dst
    if args and args == "install" then
        local installdir = target:installdir()
        if not installdir then
            print("project installdir is not set. please config project first")
            return -1
        end
        dst = path.absolute(path.join(installdir, "bin", conf.dst_dir, config.mode()))
        if not os.isdir(dst) then
            print("project not installed, please run `xmake i -o xxx_target_path` first")
            return -1
        end
    else
        local targetdir = target:targetdir()
        if not targetdir then
            print("project targetdir is not set. please config project first")
            return -1
        end
        dst = path.absolute(path.join(targetdir, conf.dst_dir, config.mode()))
        if not os.isdir(dst) then
            print("project not build, please run `xmake` first")
            return -1
        end
    end
    print("copy to  ", dst)
    helper.copy_dir_if_newer_recursive(src, dst)
end
