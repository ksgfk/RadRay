file(GLOB RADRAY_IMGUI_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB RADRAY_IMGUI_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radrayimgui STATIC ${RADRAY_IMGUI_SRC} ${RADRAY_IMGUI_INC})
target_include_directories(radrayimgui PUBLIC include)
target_link_libraries(radrayimgui PUBLIC
    radraycore)
target_compile_definitions(radrayimgui PUBLIC
    RADRAY_ENABLE_IMGUI)
radray_set_build_path(radrayimgui)
