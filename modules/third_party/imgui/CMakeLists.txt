file(GLOB _IMGUI_SRC 
    "src/imgui.cpp"
    "src/imgui_widgets.cpp"
    "src/imgui_tables.cpp"
    "src/imgui_draw.cpp"
    "src/imgui_demo.cpp"
    "src/imgui_stdlib.cpp")
file(GLOB _IMGUI_INC CONFIGURE_DEPENDS "include/*.h")

add_library(radrayimguiraw STATIC ${_IMGUI_SRC} ${_IMGUI_INC})
target_sources(radrayimguiraw PRIVATE
    $<$<PLATFORM_ID:Windows>:src/imgui_impl_win32.cpp>)
target_include_directories(radrayimguiraw PUBLIC include)
target_link_libraries(radrayimguiraw PUBLIC
    radraycore)
if (RADRAY_ENABLE_FREETYPE)
    target_sources(radrayimguiraw PRIVATE "src/misc/freetype/imgui_freetype.cpp")
    target_link_libraries(radrayimguiraw PRIVATE freetype)
    target_compile_definitions(radrayimguiraw PRIVATE IMGUI_ENABLE_FREETYPE)
endif()
target_compile_definitions(radrayimguiraw PUBLIC
    ImTextureID=ImU64
    $<$<BOOL:${RADRAY_DISABLE_IMGUI_DEMO}>:IMGUI_DISABLE_DEMO_WINDOWS>)
if (MSVC)
    target_compile_options(radrayimguiraw PUBLIC /permissive- /utf-8 /Zc:preprocessor /Zc:__cplusplus)
    target_link_options(radrayimguiraw PUBLIC /NATVIS:${CMAKE_CURRENT_LIST_DIR}/src/misc/debuggers/imgui.natvis)
endif()
radray_optimize_flags_library(radrayimguiraw)
radray_set_build_path(radrayimguiraw)
