if (RADRAY_ENABLE_DXC)
    radray_add_example(hello_deferred_shading
        SOURCES hello_deferred_shading.cpp
        LINK_LIBS radraywindow radrayrender radrayimgui
        COMPILE_OPTIONS $<$<BOOL:${MSVC}>:/W4>)

    radray_example_files(hello_deferred_shading
        ${CMAKE_CURRENT_SOURCE_DIR}/deferred_gbuffer.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/deferred_lighting.hlsl
        ${CMAKE_CURRENT_SOURCE_DIR}/deferred_tonemap.hlsl)
endif()
