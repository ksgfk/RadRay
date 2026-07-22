#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <radray/render/common.h>
#include <radray/runtime/asset.h>
#include <radray/runtime/asset_manager.h>
#include <radray/shader/shader_binary.h>

namespace radray {

using render::ShaderColorTargetDesc;
using render::ShaderComputePassDesc;
using render::ShaderGraphicsPassDesc;
using render::ShaderKeywordGroupDesc;
using render::ShaderKeywordScope;
using render::ShaderPassDesc;
using render::ShaderPassProgramDesc;
using render::ShaderStencilTestDesc;
using render::ShaderTagDesc;
using render::ShaderVariantKey;
using render::ShaderVariantDesc;
using render::ShaderVariantDomain;
using render::ShaderBakeSet;

render::CompareFunction ToRenderCompareFunction(render::CompareFunction value) noexcept;
render::CullMode ToRenderCullMode(render::CullMode value) noexcept;
render::StencilOperation ToRenderStencilOperation(render::StencilOperation value) noexcept;
render::BlendFactor ToRenderBlendFactor(render::BlendFactor value) noexcept;
render::BlendOperation ToRenderBlendOperation(render::BlendOperation value) noexcept;
render::ColorWrites ToRenderColorWrites(render::ColorWrites value) noexcept;
render::BlendComponent ToRenderBlendComponent(const render::BlendComponent& value) noexcept;
render::BlendState ToRenderBlendState(const render::BlendState& value) noexcept;
render::StencilFaceState ToRenderStencilFaceState(const render::StencilFaceState& value) noexcept;
render::StencilState ToRenderStencilState(const render::StencilState& value) noexcept;

class ShaderAsset final : public Asset {
public:
    ShaderAsset() noexcept;
    explicit ShaderAsset(vector<ShaderPassDesc> passes) noexcept;
    explicit ShaderAsset(render::ShaderBinary binary) noexcept;
    ~ShaderAsset() noexcept override;

    void OnUnload(IRenderResourceRecycler& recycler) override;
    AssetTypeId GetTypeId() const noexcept override;

    bool IsValid() const noexcept;
    const vector<ShaderPassDesc>& GetPasses() const noexcept { return _binary.Asset.Passes; }
    const render::ShaderBinary& GetBinary() const noexcept { return _binary; }

    Nullable<const render::ShaderStageArtifact*> FindCompiledStage(
        render::ShaderTarget target,
        uint32_t passIndex,
        render::ShaderStage stage,
        const vector<string>& defines) const noexcept;
    Nullable<const render::ShaderProgramVariantArtifact*> FindProgramVariant(
        render::ShaderTarget target,
        uint32_t passIndex,
        const vector<string>& defines) const noexcept;

    std::optional<uint32_t> FindPassByName(std::string_view name) const noexcept;
    std::optional<uint32_t> FindPassByTag(std::string_view name, std::string_view value) const noexcept;

private:
    render::ShaderBinary _binary;
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
