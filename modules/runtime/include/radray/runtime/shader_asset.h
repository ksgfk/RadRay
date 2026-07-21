#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <radray/render/common.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/shader/shader_binary.h>

namespace radray {

using shader::ShaderColorTargetDesc;
using shader::ShaderComputePassDesc;
using shader::ShaderGraphicsPassDesc;
using shader::ShaderKeywordGroupDesc;
using shader::ShaderKeywordScope;
using shader::ShaderPassDesc;
using shader::ShaderPassProgramDesc;
using shader::ShaderStencilTestDesc;
using shader::ShaderTagDesc;
using shader::ShaderVariantKey;
using shader::ShaderVariantDesc;
using shader::ShaderVariantDomain;
using shader::ShaderBakeSet;

render::CompareFunction ToRenderCompareFunction(shader::CompareFunction value) noexcept;
render::CullMode ToRenderCullMode(shader::CullMode value) noexcept;
render::StencilOperation ToRenderStencilOperation(shader::StencilOperation value) noexcept;
render::BlendFactor ToRenderBlendFactor(shader::BlendFactor value) noexcept;
render::BlendOperation ToRenderBlendOperation(shader::BlendOperation value) noexcept;
render::ColorWrites ToRenderColorWrites(shader::ColorWrites value) noexcept;
render::BlendComponent ToRenderBlendComponent(const shader::BlendComponent& value) noexcept;
render::BlendState ToRenderBlendState(const shader::BlendState& value) noexcept;
render::StencilFaceState ToRenderStencilFaceState(const shader::StencilFaceState& value) noexcept;
render::StencilState ToRenderStencilState(const shader::StencilState& value) noexcept;

class ShaderAsset final : public Asset {
public:
    ShaderAsset() noexcept;
    explicit ShaderAsset(vector<ShaderPassDesc> passes) noexcept;
    explicit ShaderAsset(shader::ShaderBinary binary) noexcept;
    ~ShaderAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept;
    const vector<ShaderPassDesc>& GetPasses() const noexcept { return _binary.Asset.Passes; }
    const shader::ShaderBinary& GetBinary() const noexcept { return _binary; }

    Nullable<const shader::ShaderStageArtifact*> FindCompiledStage(
        shader::ShaderTarget target,
        uint32_t passIndex,
        shader::ShaderStage stage,
        const vector<string>& defines) const noexcept;
    Nullable<const shader::ShaderProgramVariantArtifact*> FindProgramVariant(
        shader::ShaderTarget target,
        uint32_t passIndex,
        const vector<string>& defines) const noexcept;

    std::optional<uint32_t> FindPassByName(std::string_view name) const noexcept;
    std::optional<uint32_t> FindPassByTag(std::string_view name, std::string_view value) const noexcept;

private:
    shader::ShaderBinary _binary;
    bool _valid{false};
};

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    const std::filesystem::path& path);

template <>
struct RuntimeTypeTrait<ShaderAsset> {
    static constexpr RuntimeTypeId value{0x1ed35d36, 0xfc77, 0x456e, 0xa9, 0x10, 0x5c, 0xa4, 0x49, 0x69, 0x57, 0xb3};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
