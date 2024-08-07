#pragma once

#include <stdexcept>

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/rhi/ctypes.h>

#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <directx/d3dx12.h>
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>

#include <string>

namespace radray::rhi::d3d12 {

class D3D12Exception : public std::runtime_error {
public:
    explicit D3D12Exception(const std::string& message) : std::runtime_error(message) {}
    explicit D3D12Exception(const char* message) : std::runtime_error(message) {}
};

using Microsoft::WRL::ComPtr;

const char* GetErrorName(HRESULT hr) noexcept;

std::wstring Utf8ToWString(const std::string& str) noexcept;

std::string Utf8ToString(const std::wstring& str) noexcept;

D3D12_COMMAND_LIST_TYPE EnumConvert(RadrayQueueType type) noexcept;

DXGI_FORMAT EnumConvert(RadrayFormat format) noexcept;

}  // namespace radray::rhi::d3d12

#ifndef RADRAY_DX_CHECK
#define RADRAY_DX_CHECK(x)                                                                                               \
    do {                                                                                                                 \
        HRESULT hr_ = (x);                                                                                               \
        if (hr_ != S_OK) [[unlikely]] {                                                                                  \
            RADRAY_ABORT("D3D12 error '{} with error {} (code = {})", #x, ::radray::rhi::d3d12::GetErrorName(hr_), hr_); \
        }                                                                                                                \
    } while (false)
#endif

#ifndef RADRAY_DX_THROW
#define RADRAY_DX_THROW(x) \
    throw D3D12Exception(x);
#endif

#ifndef RADRAY_DX_FTHROW
#define RADRAY_DX_FTHROW(x)                                                                                                           \
    do {                                                                                                                              \
        HRESULT hr_ = (x);                                                                                                            \
        if (hr_ != S_OK) [[unlikely]] {                                                                                               \
            auto mfmt__ = std::format("D3D12 error '{} with error {} (code = {})", #x, ::radray::rhi::d3d12::GetErrorName(hr_), hr_); \
            throw D3D12Exception(mfmt__);                                                                                             \
        }                                                                                                                             \
    } while (false)
#endif
