file(GLOB_RECURSE RADRAY_RUNTIME_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB_RECURSE RADRAY_RUNTIME_SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(radrayruntime STATIC ${RADRAY_RUNTIME_SRC} ${RADRAY_RUNTIME_INC})
target_include_directories(radrayruntime PUBLIC include)
target_link_libraries(radrayruntime PUBLIC
    radraycore
    radrayrender
    radraywindow)
radray_default_compile_flags(radrayruntime)
radray_optimize_flags_library(radrayruntime)
radray_set_build_path(radrayruntime)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
