file(GLOB RADRAY_RUNTIME_INC CONFIGURE_DEPENDS "include/radray/runtime/*.h")
file(GLOB RADRAY_RUNTIME_SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(radrayruntime STATIC ${RADRAY_RUNTIME_SRC} ${RADRAY_RUNTIME_INC})
add_library(RadRay::runtime ALIAS radrayruntime)
set_target_properties(radrayruntime PROPERTIES EXPORT_NAME runtime)
target_include_directories(radrayruntime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(radrayruntime PUBLIC RadRay::render)
radray_default_compile_flags(radrayruntime)
radray_optimize_flags_library(radrayruntime)
radray_set_build_path(radrayruntime)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
