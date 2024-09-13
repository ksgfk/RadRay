#pragma once

#include <stdexcept>
#include <string_view>

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/rhi/ctypes.h>
#include <radray/rhi/helper.h>

#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <directx/d3dx12.h>
#include <directx/d3d12shader.h>
#include <dxcapi.h>
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>

namespace radray::rhi::d3d12 {

class D3D12Exception : public std::runtime_error {
public:
    explicit D3D12Exception(const radray::string& message) : std::runtime_error(message.c_str()) {}
    explicit D3D12Exception(const char* message) : std::runtime_error(message) {}
};

using Microsoft::WRL::ComPtr;

const char* GetErrorName(HRESULT hr) noexcept;

radray::wstring Utf8ToWString(const radray::string& str) noexcept;

radray::string Utf8ToString(const radray::wstring& str) noexcept;

void SetObjectName(std::string_view str, ID3D12Object* obj, D3D12MA::Allocation* alloc = nullptr) noexcept;

UINT SubresourceIndex(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize) noexcept;

D3D12_COMMAND_LIST_TYPE EnumConvert(RadrayQueueType type) noexcept;
DXGI_FORMAT EnumConvert(RadrayFormat format) noexcept;
D3D12_RESOURCE_STATES EnumConvert(RadrayResourceStates state) noexcept;
D3D12_HEAP_TYPE EnumConvert(RadrayHeapUsage usage) noexcept;
D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE EnumConvert(RadrayLoadAction load) noexcept;
D3D12_RENDER_PASS_ENDING_ACCESS_TYPE EnumConvert(RadrayStoreAction store) noexcept;

DXGI_FORMAT TypelessFormat(DXGI_FORMAT fmt) noexcept;

std::string_view to_string(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept;

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
#define RADRAY_DX_THROW(fmt, ...) \
    throw D3D12Exception(radray::format(fmt __VA_OPT__(, ) __VA_ARGS__));
#endif

#ifndef RADRAY_DX_FTHROW
#define RADRAY_DX_FTHROW(x)                                                                                                                \
    do {                                                                                                                                   \
        HRESULT hr_ = (x);                                                                                                                 \
        if (hr_ != S_OK) [[unlikely]] {                                                                                                    \
            auto mfmt__ = ::radray::format("D3D12 error '{} with error {} (code = {})", #x, ::radray::rhi::d3d12::GetErrorName(hr_), hr_); \
            throw D3D12Exception(mfmt__);                                                                                                  \
        }                                                                                                                                  \
    } while (false)
#endif

template <class CharT>
struct fmt::formatter<D3D12_DESCRIPTOR_HEAP_TYPE, CharT> : fmt::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(D3D12_DESCRIPTOR_HEAP_TYPE const& val, FormatContext& ctx) const {
        return formatter<std::string_view, CharT>::format(radray::rhi::d3d12::to_string(val), ctx);
    }
};
