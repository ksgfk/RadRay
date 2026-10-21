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

radray_add_test(test_radray_core
    SOURCES ${radray_core_test_sources}
    LINK_LIBS radraycore
    DISCOVER_ARGS PROPERTIES ENVIRONMENT "RADRAY_ASSETS_DIR=${CMAKE_SOURCE_DIR}/assets")

radray_add_test(test_inline_vector SOURCES test_inline_vector.cpp LINK_LIBS radraycore)
