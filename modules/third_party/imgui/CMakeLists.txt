file(GLOB _IMGUI_SRC 
    "src/imgui.cpp"
    "src/imgui_widgets.cpp"
    "src/imgui_tables.cpp"
    "src/imgui_draw.cpp"
    "src/imgui_demo.cpp")
file(GLOB _IMGUI_INC CONFIGURE_DEPENDS "include/*.h")

add_library(imgui STATIC ${_IMGUI_SRC} ${_IMGUI_INC})
target_sources(imgui PRIVATE
    $<$<PLATFORM_ID:Windows>:src/imgui_impl_win32.cpp>)
target_include_directories(imgui PUBLIC include)
target_link_libraries(imgui PUBLIC
    radraycore)
if (RADRAY_ENABLE_FREETYPE)
    target_sources(imgui PRIVATE "src/misc/freetype/imgui_freetype.cpp")
    target_link_libraries(imgui PRIVATE freetype)
    target_compile_definitions(imgui PRIVATE IMGUI_ENABLE_FREETYPE)
endif()
target_compile_definitions(imgui PUBLIC ImTextureID=ImU64)
if (MSVC)
    target_compile_options(radraycore PUBLIC /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus)
endif()
radray_optimize_flags_library(imgui)
radray_set_build_path(imgui)
