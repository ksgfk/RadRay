file(GLOB RADRAY_IMGUI_SRC CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB RADRAY_IMGUI_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radrayimgui STATIC ${RADRAY_IMGUI_SRC} ${RADRAY_IMGUI_INC})
target_include_directories(radrayimgui PUBLIC include)
target_link_libraries(radrayimgui PUBLIC
    radraycore)
target_compile_definitions(radrayimgui PUBLIC
    RADRAY_ENABLE_IMGUI)
if (RADRAY_ENABLE_FREETYPE)
    target_sources(radrayimgui PRIVATE "src/misc/freetype/imgui_freetype.cpp")
    target_link_libraries(radrayimgui PRIVATE freetype)
    target_compile_definitions(radrayimgui PRIVATE IMGUI_ENABLE_FREETYPE)
endif()
if (MSVC)
    target_compile_options(radraycore PUBLIC /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus)
endif()
radray_set_build_path(radrayimgui)
