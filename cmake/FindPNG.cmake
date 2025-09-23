# 若已有目标，直接返回
if(TARGET PNG::PNG)
  set(PNG_FOUND TRUE)
  # 兼容变量（尽量从目标读）
  get_target_property(_png_includes PNG::PNG INTERFACE_INCLUDE_DIRECTORIES)
  if(_png_includes)
    set(PNG_PNG_INCLUDE_DIR "${_png_includes}")
    set(PNG_INCLUDE_DIRS "${_png_includes}")
  endif()
  return()
endif()

# 使用子项目生成的 png_static
if(TARGET png_static)
  set(_png_impl png_static)

  add_library(PNG::PNG INTERFACE IMPORTED)
  target_link_libraries(PNG::PNG INTERFACE ${_png_impl})

  get_target_property(_png_includes ${_png_impl} INTERFACE_INCLUDE_DIRECTORIES)
  if(_png_includes)
    set(PNG_PNG_INCLUDE_DIR "${_png_includes}")
    set(PNG_INCLUDE_DIRS "${_png_includes}")
  endif()
  # 一些脚本用这两个变量；给出目标名即可（路径不是必须）
  set(PNG_LIBRARY ${_png_impl})
  set(PNG_LIBRARIES ${_png_impl})

  set(PNG_FOUND TRUE)
  mark_as_advanced(PNG_PNG_INCLUDE_DIR PNG_LIBRARY)
  return()
endif()

# 回落到 CMake 自带的查找逻辑
include("${CMAKE_ROOT}/Modules/FindPNG.cmake")