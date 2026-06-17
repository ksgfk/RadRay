radray_add_example(gltf_viewer SOURCES gltf_viewer.cpp LINK_LIBS radraycore radrayrender radrayruntime radraywindow cgltf::cgltf)
radray_example_files(gltf_viewer gltf_viewer.hlsl)
target_compile_definitions(gltf_viewer PRIVATE "RADRAY_GLTF_VIEWER_ASSET_DIR=\"${CMAKE_SOURCE_DIR}/assets/gltf_viewer\"")
