#pragma once

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/rhi/common.h>
#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <directx/d3dx12.h>
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>

#include <string>

namespace radray::rhi::d3d12 {

using Microsoft::WRL::ComPtr;

const char* GetErrorName(HRESULT hr) noexcept;

std::wstring Utf8ToWString(const std::string& str) noexcept;

std::string Utf8ToString(const std::wstring& str) noexcept;

uint32_t DxgiFormatByteSize(DXGI_FORMAT format) noexcept;

D3D12_COMMAND_LIST_TYPE ToCmdListType(CommandListType type) noexcept;

D3D12_HEAP_TYPE ToHeapType(BufferType type) noexcept;

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
