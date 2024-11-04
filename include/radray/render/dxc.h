#pragma once

#include <span>

#include <radray/render/common.h>

#ifdef RADRAY_ENABLE_DXC

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

class DxilBlob {
public:
    radray::vector<byte> data;
    radray::vector<byte> refl;
};

class Dxc : public RenderBase {
public:
    ~Dxc() noexcept override;

    bool IsValid() const noexcept override { return _impl != nullptr; }
    void Destroy() noexcept override;

    std::optional<DxilBlob> Compile(std::string_view code, std::span<std::string_view> args) noexcept;
    std::optional<DxilBlob> Compile(
        std::string_view code,
        std::string_view entryPoint,
        ShaderStage stage,
        HlslShaderModel sm,
        bool isOptimize,
        std::span<std::string_view> defines = {},
        std::span<std::string_view> includes = {}) noexcept;

private:
    class Impl;

    Impl* _impl{nullptr};

    friend class Impl;
    friend std::optional<std::shared_ptr<Dxc>> CreateDxc() noexcept;
};

std::optional<std::shared_ptr<Dxc>> CreateDxc() noexcept;

}  // namespace radray::render

#endif
