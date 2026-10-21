file(GLOB_RECURSE RADRAY_CORE_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_CORE_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radraycore STATIC ${RADRAY_CORE_SRC} ${RADRAY_CORE_INC})
add_library(RadRay::core ALIAS radraycore)
set_target_properties(radraycore PROPERTIES EXPORT_NAME core)
target_include_directories(radraycore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(radraycore PUBLIC cxx_std_20)
target_compile_definitions(radraycore PUBLIC
    $<$<PLATFORM_ID:Windows>:RADRAY_PLATFORM_WINDOWS>
    $<$<PLATFORM_ID:Darwin>:RADRAY_PLATFORM_MACOS>
    $<$<PLATFORM_ID:iOS>:RADRAY_PLATFORM_IOS>
    $<$<OR:$<PLATFORM_ID:Darwin>,$<PLATFORM_ID:iOS>>:RADRAY_PLATFORM_APPLE>
    $<$<NOT:$<CONFIG:Release>>:RADRAY_IS_DEBUG>)
target_link_libraries(radraycore PUBLIC
    fmt::fmt
    magic_enum::magic_enum
    Eigen3::Eigen
    Pal::Sigslot
    STDEXEC::stdexec
    $<$<BOOL:${RADRAY_ENABLE_MIMALLOC}>:$<LINK_LIBRARY:WHOLE_ARCHIVE,mimalloc-static>>
    $<$<BOOL:${RADRAY_ENABLE_PROFILER}>:Tracy::TracyClient>)
target_link_libraries(radraycore PRIVATE
    spdlog::spdlog
    xxHash::xxhash
    yyjson::yyjson
    pugixml::static
    $<$<PLATFORM_ID:Windows>:ole32>
    $<$<BOOL:${RADRAY_ENABLE_LIBPNG}>:PNG::PNG>
    $<$<BOOL:${RADRAY_ENABLE_LIBJPEG}>:JPEG::JPEG>)
target_compile_definitions(radraycore PUBLIC
    $<$<BOOL:${RADRAY_ENABLE_MIMALLOC}>:RADRAY_ENABLE_MIMALLOC>
    $<$<BOOL:${RADRAY_ENABLE_PROFILER}>:RADRAY_ENABLE_PROFILER>
    $<$<BOOL:${RADRAY_ENABLE_LIBPNG}>:RADRAY_ENABLE_PNG>
    $<$<BOOL:${RADRAY_ENABLE_LIBJPEG}>:RADRAY_ENABLE_JPEG>)
radray_default_compile_flags(radraycore)
radray_optimize_flags_library(radraycore)
radray_set_build_path(radraycore)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
