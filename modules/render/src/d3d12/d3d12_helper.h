#pragma once

#include <optional>

#include <radray/types.h>
#include <radray/logger.h>

#include <windows.h>
#include <wrl.h>
#include <dxgi1_6.h>
#include <directx/d3dx12.h>
#include <directx/d3d12shader.h>
#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include <D3D12MemAlloc.h>

namespace radray::render::d3d12 {

using Microsoft::WRL::ComPtr;

class Win32Event {
public:
    Win32Event() noexcept = default;
    ~Win32Event() noexcept;
    Win32Event(const Win32Event& other) noexcept = delete;
    Win32Event(Win32Event&& other) noexcept;
    Win32Event& operator=(Win32Event&& other) noexcept;
    Win32Event& operator=(const Win32Event& other) noexcept = delete;

    HANDLE Get() const noexcept { return _event; }

    void Destroy() noexcept;

private:
    HANDLE _event{nullptr};

    friend std::optional<Win32Event> MakeWin32Event() noexcept;
};

}  // namespace radray::render::d3d12
