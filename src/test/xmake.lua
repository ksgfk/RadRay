target("test_str_convert")
    add_rules("radray_basic_setting", "radray_test")
    add_files("test_str_convert.cpp", "test_main.cpp")
    add_deps("radray_core")
    add_tests("default")
target_end()

target("test_byte_to_dword")
    add_rules("radray_basic_setting", "radray_test")
    add_files("test_byte_to_dword.cpp", "test_main.cpp")
    add_deps("radray_core")
    add_tests("default")
target_end()

target("test_buddy_alloc")
    add_rules("radray_basic_setting", "radray_test")
    add_files("test_buddy_alloc.cpp", "test_main.cpp")
    add_deps("radray_core")
    add_tests("default")
target_end()

target("test_free_list_alloc")
    add_rules("radray_basic_setting", "radray_test")
    add_files("test_free_list_alloc.cpp", "test_main.cpp")
    add_deps("radray_core")
    add_tests("default")
target_end()

target("test_block_alloc")
    add_rules("radray_basic_setting", "radray_test")
    add_files("test_block_alloc.cpp", "test_main.cpp")
    add_deps("radray_core")
    add_tests("default")
target_end()

target("test_wavefront_obj")
    add_rules("radray_basic_setting", "radray_test")
    add_files("test_wavefront_obj.cpp", "test_main.cpp")
    add_deps("radray_core")
    add_tests("default")
target_end()

includes("test_img_rw")

includes("hello_world_dx12")
includes("hello_world_vk")
includes("direct_light")

if is_mode("release") then
    includes("bench_read_obj")
end
