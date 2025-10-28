file(GLOB RADRAY_IMGUI_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_IMGUI_SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(radrayimgui STATIC ${RADRAY_IMGUI_SRC} ${RADRAY_IMGUI_INC})
target_link_libraries(radrayimgui PUBLIC
    radraycore
    radraywindow
    radrayrender
    radrayimguiraw)
target_include_directories(radrayimgui PUBLIC include)
target_compile_options(radrayimgui PRIVATE
    $<$<BOOL:MSVC>:/W4>)
target_compile_definitions(radrayimgui PUBLIC
    RADRAY_ENABLE_IMGUI)
radray_optimize_flags_library(radrayimgui)
radray_set_build_path(radrayimgui)
