file(GLOB_RECURSE RADRAY_RUNTIME_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB_RECURSE RADRAY_RUNTIME_SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(radrayruntime STATIC ${RADRAY_RUNTIME_SRC} ${RADRAY_RUNTIME_INC})
target_include_directories(radrayruntime PUBLIC include)
target_link_libraries(radrayruntime PUBLIC
    radraycore
    radrayrender)
if (WIN32)
    target_compile_definitions(radrayruntime PRIVATE UNICODE _UNICODE)
endif()
if (MSVC)
    target_compile_definitions(radrayruntime PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
target_compile_options(radrayruntime PRIVATE
    $<$<BOOL:${MSVC}>:/W4>)
radray_optimize_flags_library(radrayruntime)
radray_set_build_path(radrayruntime)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
