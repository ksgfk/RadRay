#pragma once

#include <vector>
#include <variant>

namespace radray::rhi {

class ICommand {
public:
    virtual ~ICommand() noexcept = default;
};

class ClearRenderTargetCommand : public ICommand {
public:
    virtual ~ClearRenderTargetCommand() noexcept = default;
};

class PresentCommand : public ICommand {
public:
    virtual ~PresentCommand() noexcept = default;
};

using Command = std::variant<
    ClearRenderTargetCommand,
    PresentCommand>;

class CommandList {
public:
private:
    std::pmr::vector<Command> _list;
};

}  // namespace radray::rhi
