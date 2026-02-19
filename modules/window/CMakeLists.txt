file(GLOB RADRAY_WINDOW_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_WINDOW_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radraywindow STATIC ${RADRAY_WINDOW_SRC} ${RADRAY_WINDOW_INC})
target_include_directories(radraywindow PUBLIC include)
if (WIN32)
    file(GLOB_RECURSE RADRAY_WINDOW_WIN32_SRC CONFIGURE_DEPENDS "src/win32/*.cpp")
    target_sources(radraywindow PRIVATE ${RADRAY_WINDOW_WIN32_SRC})
endif()
target_link_libraries(radraywindow PUBLIC
    radraycore)
target_compile_options(radraywindow PRIVATE
    $<$<BOOL:${MSVC}>:/W4>)
radray_optimize_flags_library(radraywindow)
radray_set_build_path(radraywindow)
