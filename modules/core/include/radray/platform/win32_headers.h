#pragma once

#if !defined(_WIN32) && !defined(_WIN64)
#error "radray/platform/win32_headers.h requires Windows."
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// rpcndr.h 把 small 定义成 char。Unity Build 会把这个宏带进同一批后面的 .cpp。
#ifdef small
#undef small
#endif

namespace radray {}
