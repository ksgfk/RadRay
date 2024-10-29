#pragma once

#include <string_view>

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>

#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <directx/d3dx12.h>
#include <directx/d3d12shader.h>
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>

namespace radray::render::d3d12 {

using Microsoft::WRL::ComPtr;

std::string_view GetErrorName(HRESULT hr) noexcept;

void SetObjectName(std::string_view str, ID3D12Object* obj, D3D12MA::Allocation* alloc = nullptr) noexcept;

UINT SubresourceIndex(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize) noexcept;

DXGI_FORMAT TypelessFormat(DXGI_FORMAT fmt) noexcept;

}  // namespace radray::render::d3d12

#ifndef RADRAY_DX_CHECK
#define RADRAY_DX_CHECK(x)                                                                                               \
    do {                                                                                                                 \
        HRESULT hr_ = (x);                                                                                               \
        if (hr_ != S_OK) [[unlikely]] {                                                                                  \
            RADRAY_ABORT("D3D12 error '{} with error {} (code = {})", #x, ::radray::rhi::d3d12::GetErrorName(hr_), hr_); \
        }                                                                                                                \
    } while (false)
#endif

std::string_view format_as(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept;
std::string_view format_as(D3D_FEATURE_LEVEL v) noexcept;
std::string_view format_as(D3D_SHADER_MODEL v) noexcept;
std::string_view format_as(D3D12_RESOURCE_HEAP_TIER v) noexcept;
std::string_view format_as(D3D12_RESOURCE_BINDING_TIER v) noexcept;
