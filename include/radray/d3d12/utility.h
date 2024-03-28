#pragma once

#ifndef UNICODE
#define UNICODE 1
#endif
#ifndef NOMINMAX
#define NOMINMAX 1
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN 1
#endif

#include <string>

#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <d3d12.h>
#include <d3dx12.h>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray::d3d12 {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

constexpr uint64 CalcConstantBufferByteSize(uint64 byteSize) noexcept {
    // Constant buffers must be a multiple of the minimum hardware
    // allocation size (usually 256 bytes).  So round up to nearest
    // multiple of 256.  We do this by adding 255 and then masking off
    // the lower 2 bytes which store all bits < 256.
    // Example: Suppose byteSize = 300.
    // (300 + 255) & ~255
    // 555 & ~255
    // 0x022B & ~0x00ff
    // 0x022B & 0xff00
    // 0x0200
    // 512
    return (byteSize + (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)) & ~(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1);
}

constexpr uint64 CalcPlacedOffsetAlignment(uint64 offset) {
    return (offset + (D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1)) & ~(D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT - 1);
}

const char* GetErrorName(HRESULT hr) noexcept;

#ifndef ThrowIfFailed
#define ThrowIfFailed(x)                                                                                            \
    do {                                                                                                            \
        HRESULT hr_ = (x);                                                                                          \
        if (hr_ != S_OK) [[unlikely]] {                                                                             \
            RADRAY_ABORT("D3D12 error '{} with error {} (code = {})", #x, ::radray::d3d12::GetErrorName(hr_), hr_); \
        }                                                                                                           \
    } while (false)
#endif

std::wstring Utf8ToWString(const std::string& str) noexcept;

std::string Utf8ToString(const std::wstring& str) noexcept;

uint32 DxgiFormatByteSize(DXGI_FORMAT format) noexcept;

}  // namespace radray::d3d12
