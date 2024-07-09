#pragma once

#include <array>
#include <vector>
#include <variant>

#include <radray/rhi/resource.h>

namespace radray::rhi {

class ICommand {
public:
    virtual ~ICommand() noexcept = default;
};

class ClearRenderTargetCommand : public ICommand {
public:
    explicit ClearRenderTargetCommand(
        SwapChainHandle swapchain,
        const std::array<float, 4>& color) noexcept
        : texture(swapchain), color(color) {}
    explicit ClearRenderTargetCommand(
        ResourceHandle texture,
        const std::array<float, 4>& color) noexcept
        : texture(texture), color(color) {}
    virtual ~ClearRenderTargetCommand() noexcept = default;

public:
    std::variant<SwapChainHandle, ResourceHandle> texture;
    std::array<float, 4> color;
};

class PresentCommand : public ICommand {
public:
    explicit PresentCommand(SwapChainHandle handle) noexcept : swapchain(handle) {}
    virtual ~PresentCommand() noexcept = default;

public:
    SwapChainHandle swapchain;
};

using Command = std::variant<
    ClearRenderTargetCommand,
    PresentCommand>;

class CommandList {
public:
public:
    std::pmr::vector<Command> list;
};

}  // namespace radray::rhi
