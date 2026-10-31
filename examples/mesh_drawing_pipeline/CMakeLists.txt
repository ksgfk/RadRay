add_executable(radray_mesh_drawing_sample
    sample.h sample.cpp
    pipeline.h pipeline.cpp
    gpu_support.h gpu_support.cpp)
target_link_libraries(radray_mesh_drawing_sample PRIVATE RadRay::runtime)
target_compile_definitions(radray_mesh_drawing_sample PRIVATE RADRAY_SAMPLE_ROOT="${RADRAY_SOURCE_ROOT}")
radray_default_compile_flags(radray_mesh_drawing_sample)
radray_optimize_flags_binary(radray_mesh_drawing_sample)
radray_set_build_path(radray_mesh_drawing_sample)
