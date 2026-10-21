file(GLOB RADRAY_WINDOW_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_WINDOW_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radraywindow STATIC ${RADRAY_WINDOW_SRC} ${RADRAY_WINDOW_INC})
add_library(RadRay::window ALIAS radraywindow)
set_target_properties(radraywindow PROPERTIES EXPORT_NAME window)
target_include_directories(radraywindow PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if (WIN32)
    target_link_libraries(radraywindow PRIVATE imm32)
    file(GLOB_RECURSE RADRAY_WINDOW_WIN32_SRC CONFIGURE_DEPENDS "src/win32/*.cpp")
    target_sources(radraywindow PRIVATE ${RADRAY_WINDOW_WIN32_SRC})
endif()
if (APPLE)
    file(GLOB_RECURSE RADRAY_WINDOW_COCOA_SRC CONFIGURE_DEPENDS
        "src/cocoa/*.m"
        "src/cocoa/*.mm")
    target_sources(radraywindow PRIVATE ${RADRAY_WINDOW_COCOA_SRC})
    target_compile_options(radraywindow PRIVATE
        "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>"
        "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    find_library(RADRAY_COCOA_FRAMEWORK Cocoa REQUIRED)
    target_link_libraries(radraywindow PUBLIC "-framework Cocoa")
endif()
target_link_libraries(radraywindow PUBLIC
    radraycore)
radray_default_compile_flags(radraywindow)
radray_optimize_flags_library(radraywindow)
radray_set_build_path(radraywindow)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
