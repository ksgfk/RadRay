import("core.platform.platform")
import("net.http")
import("utils.archive")

local extDir = path.absolute(path.join(os.scriptdir(), "..", "ext"))
if not os.isdir(extDir) then
    os.mkdir(extDir)
end
local buildScriptDir = path.absolute(path.join(os.scriptdir(), "ext_build"))
local includeDirs = {}
print("ext dir", extDir)

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
    if os.isdir(incDir) then
        os.rmdir(incDir)
    end
    os.mkdir(incDir)
    os.cp(path.join(srcDir, "Eigen"), path.join(incDir, "include", "Eigen"))
    copy_xmake("eigen", incDir, "eigen_include")
    print("checked", incDir, branch)
end

local function check_zlib(branch)
    check_git("https://github.com/madler/zlib.git", "zlib", branch)
    local srcDir = path.join(extDir, "zlib")
    local incDir = path.join(extDir, "zlib_include")
    if os.isdir(incDir) then
        os.rmdir(incDir)
    end
    os.cp(path.join(srcDir, "zlib.h"), path.join(incDir, "include", "zlib.h"))
    os.cp(path.join(srcDir, "zconf.h"), path.join(incDir, "include", "zconf.h"))
end

local function check_libpng(branch)
    check_git("https://github.com/pnggroup/libpng.git", "libpng", branch)
    local srcDir = path.join(extDir, "libpng")
    local incDir = path.join(extDir, "libpng_include")
    if os.isdir(incDir) then
        os.rmdir(incDir)
    end
    os.cp(path.join(srcDir, "scripts", "pnglibconf.h.prebuilt"), path.join(incDir, "include", "pnglibconf.h"))
    os.cp(path.join(srcDir, "*.h"), path.join(incDir, "include"))
end

local function check_libjpeg(branch)
    print("---------> check_libjpeg <---------")
    local srcDir = path.join(extDir, "libjpeg")
    if os.isdir(srcDir) then
        os.rmdir(srcDir)
    end
    os.mkdir(srcDir)
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
    if os.isdir(incDir) then
        os.rmdir(incDir)
    end
    os.mkdir(incDir)
    os.cd(incDir)
    local headers = {"jerror.h", "jmorecfg.h", "jpeglib.h", "jconfig.h"}
    for index, i in ipairs(headers) do
        os.cp(path.join(srcDir, i), path.join(incDir, "include", i))
    end
    copy_xmake("libjpeg", srcDir, "libjpeg")
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
gen_ext_xmake_script()
