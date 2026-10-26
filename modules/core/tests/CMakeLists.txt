# test_inline_vector.cpp 替换全局 operator new/delete，不能和其它用例共享进程映像。
set(radray_core_test_sources
    test_buddy_alloc.cpp
    test_basic_math.cpp
    test_first_fit_alloc.cpp
    test_wavefront_obj.cpp
    test_str_convert.cpp
    test_nullable.cpp
    test_intrusive_ptr.cpp
    test_enum_flags.cpp
    test_sparse_set.cpp
    test_manual_coroutine_scheduler.cpp
    test_structured_buffer.cpp
    test_pod_hash.cpp
    test_runtime_type.cpp
    test_binary_io.cpp
    test_json.cpp
    test_json_serializer.cpp
    test_json_deserializer.cpp)
if (RADRAY_ENABLE_LIBPNG)
    list(APPEND radray_core_test_sources test_img_rw.cpp)
endif()
if (WIN32 AND RADRAY_ENABLE_MIMALLOC AND RADRAY_DEPENDENCY_TRIPLET MATCHES "-staticcrt$")
    list(APPEND radray_core_test_sources test_memory_override.cpp)
endif()

add_executable(test_radray_core ${radray_core_test_sources})
target_link_libraries(test_radray_core PRIVATE radraycore GTest::gtest_main)
radray_default_compile_flags(test_radray_core)
radray_optimize_flags_binary(test_radray_core)
radray_set_build_path(test_radray_core)
gtest_discover_tests(test_radray_core
    PROPERTIES ENVIRONMENT "RADRAY_ASSETS_DIR=${RADRAY_SOURCE_ROOT}/assets")

# This executable owns new/delete. Build only its header and logging dependencies,
# without radraycore's transitive mimalloc override.
add_executable(test_inline_vector test_inline_vector.cpp ../src/logger.cpp)
target_include_directories(test_inline_vector PRIVATE ../include)
target_compile_features(test_inline_vector PRIVATE cxx_std_20)
target_compile_definitions(test_inline_vector PRIVATE $<$<NOT:$<CONFIG:Release>>:RADRAY_IS_DEBUG>)
target_link_libraries(test_inline_vector PRIVATE fmt::fmt spdlog::spdlog GTest::gtest_main)
radray_default_compile_flags(test_inline_vector)
radray_optimize_flags_binary(test_inline_vector)
radray_set_build_path(test_inline_vector)
gtest_discover_tests(test_inline_vector)
