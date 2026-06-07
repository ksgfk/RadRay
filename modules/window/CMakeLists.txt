file(GLOB RADRAY_WINDOW_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_WINDOW_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radraywindow STATIC ${RADRAY_WINDOW_SRC} ${RADRAY_WINDOW_INC})
target_include_directories(radraywindow PUBLIC include)
if (WIN32)
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
    target_link_libraries(radraywindow PUBLIC ${RADRAY_COCOA_FRAMEWORK})
endif()
target_link_libraries(radraywindow PUBLIC
    radraycore)
radray_default_compile_flags(radraywindow)
radray_optimize_flags_library(radraywindow)
radray_set_build_path(radraywindow)
