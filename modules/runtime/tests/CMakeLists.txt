if (RADRAY_BUILD_WINDOW)
    radray_add_test(test_gpu_runtime_smoke SOURCES test_gpu_runtime_smoke.cpp LINK_LIBS radraycore radrayrender radrayruntime radraywindow)
endif()
