set(RADRAY_IMGUI_ROOT "${RADRAY_THIRDPARTY_ROOT}/imgui")

file(GLOB DEAR_IMGUI_SRC 
    "${RADRAY_IMGUI_ROOT}/imgui.cpp"
    "${RADRAY_IMGUI_ROOT}/imgui_widgets.cpp"
    "${RADRAY_IMGUI_ROOT}/imgui_tables.cpp"
    "${RADRAY_IMGUI_ROOT}/imgui_draw.cpp"
    "${RADRAY_IMGUI_ROOT}/imgui_demo.cpp"
    "${RADRAY_IMGUI_ROOT}/misc/cpp/imgui_stdlib.cpp")
file(GLOB DEAR_IMGUI_INC 
    "${RADRAY_IMGUI_ROOT}/imgui.h"
    "${RADRAY_IMGUI_ROOT}/imgui_internal.h"
    "${RADRAY_IMGUI_ROOT}/imstb_rectpack.h"
    "${RADRAY_IMGUI_ROOT}/imstb_textedit.h"
    "${RADRAY_IMGUI_ROOT}/imstb_truetype.h"
    "${RADRAY_IMGUI_ROOT}/misc/cpp/imgui_stdlib.h")
file(GLOB DEAR_IMGUI_OTHER
    $<$<PLATFORM_ID:Windows>:${RADRAY_IMGUI_ROOT}/misc/debuggers/imgui.natvis>)
file(GLOB_RECURSE RADRAY_IMGUI_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_IMGUI_SRC CONFIGURE_DEPENDS "src/*.cpp")

add_library(radrayimgui STATIC
    ${DEAR_IMGUI_SRC}
    ${DEAR_IMGUI_INC}
    ${RADRAY_IMGUI_SRC} 
    ${RADRAY_IMGUI_INC}
    $<$<BOOL:${RADRAY_ENABLE_FREETYPE}>:${RADRAY_IMGUI_ROOT}/misc/freetype/imgui_freetype.h>
    $<$<BOOL:${RADRAY_ENABLE_FREETYPE}>:${RADRAY_IMGUI_ROOT}/misc/freetype/imgui_freetype.cpp>
    $<$<PLATFORM_ID:Windows>:${RADRAY_IMGUI_ROOT}/backends/imgui_impl_win32.h>
    $<$<PLATFORM_ID:Windows>:${RADRAY_IMGUI_ROOT}/backends/imgui_impl_win32.cpp>)

target_include_directories(radrayimgui PUBLIC
    include
    "${RADRAY_IMGUI_ROOT}"
    "${RADRAY_IMGUI_ROOT}/backends"
    "${RADRAY_IMGUI_ROOT}/misc/cpp"
    $<$<BOOL:${RADRAY_ENABLE_FREETYPE}>:${RADRAY_IMGUI_ROOT}/misc/freetype>)

target_link_libraries(radrayimgui PUBLIC
    radraycore
    radraywindow
    radrayrender)
target_link_libraries(radrayimgui PRIVATE
    $<$<BOOL:${RADRAY_ENABLE_FREETYPE}>:freetype>)

target_compile_definitions(radrayimgui PUBLIC
    RADRAY_ENABLE_IMGUI
    ImTextureID=ImU64
    IMGUI_USER_CONFIG="radray/imgui/imgui_config.h"
    $<$<BOOL:${RADRAY_DISABLE_IMGUI_DEMO}>:IMGUI_DISABLE_DEMO_WINDOWS>)
target_compile_definitions(radrayimgui PRIVATE
    $<$<BOOL:${RADRAY_ENABLE_FREETYPE}>:IMGUI_ENABLE_FREETYPE>)

target_compile_options(radrayimgui PRIVATE
    $<$<BOOL:MSVC>:/W4>)
radray_optimize_flags_library(radrayimgui)
radray_set_build_path(radrayimgui)
