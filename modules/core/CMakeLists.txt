file(GLOB_RECURSE RADRAY_CORE_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_CORE_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radraycore STATIC ${RADRAY_CORE_SRC} ${RADRAY_CORE_INC})
target_include_directories(radraycore PUBLIC include)
target_link_libraries(radraycore PUBLIC
    fmt::fmt
    Eigen3::Eigen
    Pal::Sigslot
    # STDEXEC::stdexec
    $<$<BOOL:${RADRAY_ENABLE_MIMALLOC}>:mimalloc-override>)
target_link_libraries(radraycore PRIVATE
    spdlog::spdlog
    xxHash::xxhash
    $<$<BOOL:${RADRAY_ENABLE_LIBPNG}>:png_static>)
target_compile_definitions(radraycore PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_MIMALLOC}>:RADRAY_ENABLE_MIMALLOC>
    $<$<BOOL:${RADRAY_ENABLE_LIBPNG}>:RADRAY_ENABLE_PNG>)
radray_default_compile_flags(radraycore)
radray_optimize_flags_library(radraycore)
radray_set_build_path(radraycore)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
