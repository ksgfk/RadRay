#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string_view>

#include <radray/render/common.h>
#include <radray/render/shader/hlsl.h>

namespace radray::render {

class DxcOutput {
public:
    vector<byte> Data;
    vector<byte> Refl;
    ShaderBlobCategory Category{ShaderBlobCategory::DXIL};
};

class DxcCompileOptions {
public:
    std::string_view EntryPoint{};
    ShaderStage Stage{};
    HlslShaderModel SM{};
    std::span<const std::string_view> Defines{};
    std::span<const std::string_view> Includes{};
    bool IsOptimize{};
    bool IsSpirv{};
    bool EnableUnbounded{};
};

}  // namespace radray::render

#ifdef RADRAY_ENABLE_DXC

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

    std::optional<DxcOutput> CompileMemory(
        std::string_view code,
        std::string_view sourceName,
        const DxcCompileOptions& options) noexcept;

    std::optional<DxcOutput> CompileFile(
        const std::filesystem::path& path,
        const DxcCompileOptions& options) noexcept;

    std::optional<HlslShaderDesc> GetShaderDescFromOutput(std::span<const byte> refl) noexcept;

private:
    unique_ptr<Impl> _impl;
};

Nullable<shared_ptr<Dxc>> CreateDxc() noexcept;

}  // namespace radray::render

#endif
