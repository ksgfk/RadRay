#pragma once

#include <radray/render/fence.h>
#include "d3d12_helper.h"

namespace radray::render::d3d12 {

class FenceD3D12 : public Fence {
public:
    FenceD3D12(
        ComPtr<ID3D12Fence> fence,
        Win32Event event) noexcept
        : _fence(std::move(fence)),
          _event(std::move(event)) {}
    ~FenceD3D12() noexcept override = default;

    bool IsValid() const noexcept override { return _fence.Get() != nullptr; }
    void Destroy() noexcept override;

public:
    ComPtr<ID3D12Fence> _fence;
    uint64_t _fenceValue{0};
    Win32Event _event{};
};

}  // namespace radray::render::d3d12
