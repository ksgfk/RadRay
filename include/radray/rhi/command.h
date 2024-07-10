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

using Command = std::variant<
    ClearRenderTargetCommand>;

class CommandList {
public:
    template <class... Args>
    void Add(Args&&... args) {
        list.emplace_back(std::forward<Args>(args)...);
    }

public:
    std::pmr::vector<Command> list;
};

}  // namespace radray::rhi
