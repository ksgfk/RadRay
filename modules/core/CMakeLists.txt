file(GLOB_RECURSE RADRAY_CORE_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_CORE_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radraycore STATIC ${RADRAY_CORE_SRC} ${RADRAY_CORE_INC})
target_include_directories(radraycore PUBLIC include)
target_link_libraries(radraycore PUBLIC
    fmt::fmt
    Eigen3::Eigen
    Pal::Sigslot
    $<$<BOOL:${RADRAY_ENABLE_MIMALLOC}>:mimalloc-static>)
target_link_libraries(radraycore PRIVATE
    spdlog::spdlog
    xxHash::xxhash
    $<$<BOOL:${RADRAY_ENABLE_LIBPNG}>:png_static>)
target_compile_definitions(radraycore PUBLIC
    $<$<PLATFORM_ID:Windows>:RADRAY_PLATFORM_WINDOWS>
    $<$<NOT:$<CONFIG:Release>>:RADRAY_IS_DEBUG>
    $<$<BOOL:${RADRAY_ENABLE_MIMALLOC}>:RADRAY_ENABLE_MIMALLOC>
    $<$<BOOL:${RADRAY_ENABLE_LIBPNG}>:RADRAY_ENABLE_PNG>)
if (MSVC)
    target_compile_options(radraycore PUBLIC /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus)
endif()
target_compile_options(radraycore PRIVATE
    $<$<BOOL:MSVC>:/W4>)
radray_compile_flag_auto_simd(radraycore)
radray_compile_flag_lto(radraycore)
radray_set_build_path(radraycore)
