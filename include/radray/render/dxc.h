#pragma once

#ifdef RADRAY_ENABLE_DXC

#include <span>

#include <radray/render/common.h>

namespace radray::render {

enum class HlslShaderModel {
    SM60,
    SM61,
    SM62,
    SM63,
    SM64,
    SM65,
    SM66
};

class DxcOutput {
public:
    radray::vector<byte> data;
    radray::vector<byte> refl;
    ShaderLangCategory category;
};

class Dxc : public RenderBase, public radray::enable_shared_from_this<Dxc> {
public:
    ~Dxc() noexcept override;

    bool IsValid() const noexcept override { return _impl != nullptr; }
    void Destroy() noexcept override;

    std::optional<DxcOutput> Compile(std::string_view code, std::span<std::string_view> args) noexcept;
    std::optional<DxcOutput> Compile(
        std::string_view code,
        std::string_view entryPoint,
        ShaderStage stage,
        HlslShaderModel sm,
        bool isOptimize,
        std::span<std::string_view> defines = {},
        std::span<std::string_view> includes = {},
        bool isSpirv = false) noexcept;

private:
    class Impl;

    Impl* _impl{nullptr};

    friend class Impl;
    friend std::optional<std::shared_ptr<Dxc>> CreateDxc() noexcept;
};

std::optional<std::shared_ptr<Dxc>> CreateDxc() noexcept;

}  // namespace radray::render

#endif
