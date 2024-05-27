#pragma once

#include <radray/platform.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/rhi/common.h>
#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <d3d12.h>

#include <string>

namespace radray::rhi::d3d12 {

using Microsoft::WRL::ComPtr;

const char* GetErrorName(HRESULT hr) noexcept;

std::wstring Utf8ToWString(const std::string& str) noexcept;

std::string Utf8ToString(const std::wstring& str) noexcept;

uint32_t DxgiFormatByteSize(DXGI_FORMAT format) noexcept;

D3D12_COMMAND_LIST_TYPE ToCmdListType(CommandListType type) noexcept;

}  // namespace radray::rhi::d3d12

#ifndef RADRAY_DX_CHECK
#define RADRAY_DX_CHECK(x)                                                                                                  \
    do {                                                                                                                 \
        HRESULT hr_ = (x);                                                                                               \
        if (hr_ != S_OK) [[unlikely]] {                                                                                  \
            RADRAY_ABORT("D3D12 error '{} with error {} (code = {})", #x, ::radray::rhi::d3d12::GetErrorName(hr_), hr_); \
        }                                                                                                                \
    } while (false)
#endif
