file(GLOB_RECURSE RADRAY_RUNTIME_INC CONFIGURE_DEPENDS "include/*.h")
file(GLOB RADRAY_RUNTIME_SRC CONFIGURE_DEPENDS "src/*.cpp")

if (RADRAY_ENABLE_IMGUI)
    if (NOT TARGET freetype)
        message(FATAL_ERROR "RADRAY_ENABLE_IMGUI requires the freetype target. Enable RADRAY_ENABLE_FREETYPE.")
    endif()

    set(RADRAY_IMGUI_ROOT "${CMAKE_SOURCE_DIR}/third_party/imgui")
    list(APPEND RADRAY_RUNTIME_SRC
        "${RADRAY_IMGUI_ROOT}/imgui.cpp"
        "${RADRAY_IMGUI_ROOT}/imgui_demo.cpp"
        "${RADRAY_IMGUI_ROOT}/imgui_draw.cpp"
        "${RADRAY_IMGUI_ROOT}/imgui_tables.cpp"
        "${RADRAY_IMGUI_ROOT}/imgui_widgets.cpp"
        "${RADRAY_IMGUI_ROOT}/misc/freetype/imgui_freetype.cpp"
        "src/imgui/radray_imgui_shader.cpp")
    if (WIN32)
        list(APPEND RADRAY_RUNTIME_SRC
            "${RADRAY_IMGUI_ROOT}/backends/imgui_impl_win32.cpp"
            "src/imgui/radray_imgui_win32.cpp")
    endif()
    list(APPEND RADRAY_RUNTIME_INC
        "include/radray/runtime/imgui/imgui_config.h"
        "${RADRAY_IMGUI_ROOT}/imgui.h"
        "${RADRAY_IMGUI_ROOT}/imconfig.h"
        "${RADRAY_IMGUI_ROOT}/imgui_internal.h"
        "${RADRAY_IMGUI_ROOT}/imstb_rectpack.h"
        "${RADRAY_IMGUI_ROOT}/imstb_textedit.h"
        "${RADRAY_IMGUI_ROOT}/misc/freetype/imgui_freetype.h")
    if (WIN32)
        list(APPEND RADRAY_RUNTIME_INC
            "${RADRAY_IMGUI_ROOT}/backends/imgui_impl_win32.h")
    endif()
endif()

add_library(radrayruntime STATIC ${RADRAY_RUNTIME_SRC} ${RADRAY_RUNTIME_INC})
target_include_directories(radrayruntime PUBLIC include)
target_link_libraries(radrayruntime PUBLIC
    radraycore
    radrayrender
    radraywindow)
if (RADRAY_ENABLE_IMGUI)
    target_include_directories(radrayruntime PUBLIC
        "${RADRAY_IMGUI_ROOT}"
        "${RADRAY_IMGUI_ROOT}/misc/freetype")
    if (WIN32)
        target_include_directories(radrayruntime PRIVATE
            "${RADRAY_IMGUI_ROOT}/backends")
    endif()
    target_link_libraries(radrayruntime PRIVATE freetype)
    target_compile_definitions(radrayruntime PUBLIC
        RADRAY_ENABLE_IMGUI
        IMGUI_USER_CONFIG=\"radray/runtime/imgui/imgui_config.h\"
        $<$<BOOL:${RADRAY_DISABLE_IMGUI_DEMO}>:IMGUI_DISABLE_DEMO_WINDOWS>)
endif()
radray_default_compile_flags(radrayruntime)
radray_optimize_flags_library(radrayruntime)
radray_set_build_path(radrayruntime)

if (RADRAY_BUILD_TESTS)
    add_subdirectory(tests)
endif()
