import("core.platform.platform")
import("net.http")
import("utils.archive")
local opts = import("options", {try = true, anonymous = true})

local options = opts.get_options()
local extDir = path.absolute(path.join(os.scriptdir(), "..", "ext"))
if not os.isdir(extDir) then
    os.mkdir(extDir)
end
local buildScriptDir = path.absolute(path.join(os.scriptdir(), "ext_build"))
local includeDirs = {}
print("ext dir", extDir)

local function remake_dir(dir)
    if os.isdir(dir) then
        os.rmdir(dir)
    end
    os.mkdir(dir)
end

local function copy_xmake(name, srcDir, incDir)
    local scriptPos = path.join(buildScriptDir, name .. ".lua")
    assert(os.isfile(scriptPos), "no xmake script " .. scriptPos)
    os.cp(scriptPos, path.join(srcDir, "xmake.lua"))
    table.insert(includeDirs, incDir)
end

local function pull_git(srcDir, url, branch)
    if os.isdir(srcDir) then
        local oldir = os.cd(srcDir)
        os.execv("git", {"fetch", "--tags"})
        os.execv("git", {"checkout", branch})
        os.execv("git", {"submodule", "update", "--init", "--recursive"})
        os.cd(oldir)
    else
        os.mkdir(srcDir)
        os.execv("git", {"clone", "--depth", "1", "-b", branch, url, srcDir})
        local oldir = os.cd(srcDir)
        os.execv("git", {"checkout", branch})
        os.cd(oldir)
    end
end

local function check_git(url, name, branch)
    print("---------> check_git <---------")
    local srcDir = path.join(extDir, name)
    pull_git(srcDir, url, branch)
    copy_xmake(name, srcDir, name)
    print("checked", srcDir, branch)
end

local function check_eigen(branch)
    print("---------> check_eigen <---------")
    local srcDir = path.join(extDir, "eigen")
    pull_git(srcDir, "https://gitlab.com/libeigen/eigen.git", branch)
    local incDir = path.join(extDir, "eigen_include")
    remake_dir(incDir)
    os.cp(path.join(srcDir, "Eigen"), path.join(incDir, "include", "Eigen"))
    copy_xmake("eigen", incDir, "eigen_include")
    print("checked", incDir, branch)
end

local function check_zlib(branch)
    check_git("https://github.com/madler/zlib.git", "zlib", branch)
    local srcDir = path.join(extDir, "zlib")
    local incDir = path.join(extDir, "zlib_include")
    remake_dir(incDir)
    os.cp(path.join(srcDir, "zlib.h"), path.join(incDir, "include", "zlib.h"))
    os.cp(path.join(srcDir, "zconf.h"), path.join(incDir, "include", "zconf.h"))
end

local function check_libpng(branch)
    check_git("https://github.com/pnggroup/libpng.git", "libpng", branch)
    local srcDir = path.join(extDir, "libpng")
    local incDir = path.join(extDir, "libpng_include")
    remake_dir(incDir)
    os.cp(path.join(srcDir, "scripts", "pnglibconf.h.prebuilt"), path.join(incDir, "include", "pnglibconf.h"))
    os.cp(path.join(srcDir, "*.h"), path.join(incDir, "include"))
end

local function check_libjpeg(branch)
    print("---------> check_libjpeg <---------")
    local srcDir = path.join(extDir, "libjpeg")
    remake_dir(srcDir)
    local olddir = os.cd(srcDir)
    local url = string.format("https://www.ijg.org/files/jpegsr%s.zip", branch)
    http.download(url, "src.zip")
    archive.extract("src.zip", srcDir)
    print("download", url)
    os.mv(path.join(srcDir, string.format("jpeg-%s", branch), "**"), srcDir)
    if os.host() == "windows" then
        os.cp("jconfig.vc", "jconfig.h")
    else
        os.cp("jconfig.txt", "jconfig.h")
    end
    local incDir = path.join(extDir, "libjpeg_include")
    remake_dir(incDir)
    os.cd(incDir)
    local headers = {"jerror.h", "jmorecfg.h", "jpeglib.h", "jconfig.h"}
    for index, i in ipairs(headers) do
        os.cp(path.join(srcDir, i), path.join(incDir, "include", i))
    end
    copy_xmake("libjpeg", srcDir, "libjpeg")
    os.cd(olddir)
end

local function check_metal_cpp(version)
    print("---------> check_metal-cpp <---------")
    local srcDir = path.join(extDir, "metal-cpp")
    remake_dir(srcDir)
    local olddir = os.cd(srcDir)
    local url = string.format("https://developer.apple.com/metal/cpp/files/metal-cpp_%s.zip", version)
    http.download(url, "src.zip")
    archive.extract("src.zip", srcDir)
    os.rm("src.zip")
    print("download", url)
    os.cd("metal-cpp")
    os.execv("python", {"SingleHeader/MakeSingleHeader.py", "Foundation/Foundation.hpp", "QuartzCore/QuartzCore.hpp", "Metal/Metal.hpp", "MetalFX/MetalFX.hpp"})
    os.cd("..")
    os.cp(path.join("metal-cpp", "SingleHeader", "Metal.hpp"), "include/Metal/")
    os.rm("metal-cpp")
    io.writefile("src/impl.cpp", [[
#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>
    ]])
    copy_xmake("metal-cpp", srcDir, "metal-cpp")
    os.cd(olddir)
end

local function gen_ext_xmake_script()
    print("---------> gen_ext_xmake_script <---------")
    local src = ""
    for i, v in ipairs(includeDirs) do
        src = src .. "includes(\"" .. v .. "\")\n"
    end
    io.writefile(path.join(extDir, "xmake.lua"), src)
    print("gen done")
end

check_git("https://github.com/gabime/spdlog.git", "spdlog", "v1.14.1")
check_eigen("3.4.0")
check_git("https://github.com/glfw/glfw.git", "glfw", "3.4")
check_zlib("v1.3.1")
check_libpng("v1.6.43")
check_libjpeg("9f")
-- check_git("https://github.com/NVIDIAGameWorks/NRI.git", "nri", "main")
if os.is_host("windows") and options.enable_d3d12 then
    check_git("https://github.com/microsoft/DirectX-Headers.git", "directx-headers", "v1.614.0")
    check_git("https://github.com/GPUOpen-LibrariesAndSDKs/D3D12MemoryAllocator.git", "d3d12ma", "v2.0.1")
end
if os.is_host("macosx") and options.enable_metal then
    check_metal_cpp("macOS14.2_iOS17.2")
end
gen_ext_xmake_script()
