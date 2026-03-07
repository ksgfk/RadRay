#pragma once

#include <array>
#include <type_traits>

#include <radray/render/common.h>
#include <radray/render/shader/hlsl.h>

namespace radray::render {

class DxcOutput {
public:
    vector<byte> Data;
    vector<byte> Refl;
    ShaderBlobCategory Category;
};

class DxcCompileParams {
public:
    std::string_view Code{};
    std::string_view EntryPoint{};
    ShaderStage Stage{};
    HlslShaderModel SM{};
    std::span<std::string_view> Defines{};
    std::span<std::string_view> Includes{};
    bool IsOptimize{};
    bool IsSpirv{};
    bool EnableUnbounded{};
};

// bool IsHlslShaderBufferEqual(const HlslShaderDesc& l, const HlslShaderBufferDesc& lcb, const HlslShaderDesc& r, const HlslShaderBufferDesc& rcb) noexcept;
// bool IsHlslTypeEqual(const HlslShaderDesc& l, size_t lType, const HlslShaderDesc& r, size_t rType) noexcept;
// std::optional<HlslShaderDesc> MergeHlslShaderDesc(std::span<const HlslShaderDesc*> descs) noexcept;

}  // namespace radray::render

#ifdef RADRAY_ENABLE_DXC

#include <span>

namespace radray::render {

class Dxc : public RenderBase, public enable_shared_from_this<Dxc> {
public:
    class Impl {
    public:
        virtual ~Impl() noexcept = default;
    };

    explicit Dxc(unique_ptr<Impl> impl) noexcept : _impl(std::move(impl)) {}
    ~Dxc() noexcept override = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }
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

    std::optional<DxcOutput> Compile(const DxcCompileParams& params) noexcept;

    std::optional<HlslShaderDesc> GetShaderDescFromOutput(std::span<const byte> refl) noexcept;

private:
    unique_ptr<Impl> _impl;
};

Nullable<shared_ptr<Dxc>> CreateDxc() noexcept;

}  // namespace radray::render

#endif
