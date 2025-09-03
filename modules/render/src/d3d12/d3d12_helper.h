#pragma once

#include <optional>

#include <radray/types.h>
#include <radray/logger.h>
#include <radray/utility.h>
#include <radray/render/common.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
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

std::optional<Win32Event> MakeWin32Event() noexcept;

std::string_view GetErrorName(HRESULT hr) noexcept;

void SetObjectName(std::string_view str, ID3D12Object* obj, D3D12MA::Allocation* alloc = nullptr) noexcept;

D3D12_COMMAND_LIST_TYPE MapType(QueueType v) noexcept;
DXGI_FORMAT MapType(TextureFormat v) noexcept;

}  // namespace radray::render::d3d12

std::string_view format_as(D3D_FEATURE_LEVEL v) noexcept;
std::string_view format_as(D3D_SHADER_MODEL v) noexcept;
std::string_view format_as(D3D12_RESOURCE_HEAP_TIER v) noexcept;
std::string_view format_as(D3D12_RESOURCE_BINDING_TIER v) noexcept;
std::string_view format_as(D3D12_DESCRIPTOR_HEAP_TYPE v) noexcept;
