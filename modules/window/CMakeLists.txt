file(GLOB_RECURSE RADRAY_WINDOW_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RADRAY_WINDOW_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radraywindow STATIC ${RADRAY_WINDOW_SRC} ${RADRAY_WINDOW_INC})
target_include_directories(radraywindow PUBLIC include)
target_link_libraries(radraywindow PUBLIC
    radraycore)
target_link_libraries(radraywindow PRIVATE
    glfw)
if (MSVC)
    target_compile_options(radraywindow PRIVATE /W4)
endif()
radray_set_build_path(radraywindow)
