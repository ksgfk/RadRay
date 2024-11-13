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
    ShaderBlobCategory category;
};

class Dxc : public RenderBase, public radray::enable_shared_from_this<Dxc> {
public:
    class Impl {
    public:
        virtual ~Impl() noexcept = default;
    };

    explicit Dxc(radray::unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
    ~Dxc() noexcept override = default;

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

    std::optional<DxilReflection> GetDxilReflection(ShaderStage stage, std::span<const byte> refl) noexcept;

private:
    radray::unique_ptr<Impl> _impl;
};

std::optional<radray::shared_ptr<Dxc>> CreateDxc() noexcept;

}  // namespace radray::render

#endif
