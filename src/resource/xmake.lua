add_requires("libpng v1.6.43", {debug = is_mode("debug")})
add_requires("libjpeg v9e", {debug = is_mode("debug")})

target("radray_resource")
    set_kind("static")
    add_rules("radray_basic_setting")
    add_files("*.cpp")
    add_deps("radray_core")
    add_packages("libpng", "libjpeg")
