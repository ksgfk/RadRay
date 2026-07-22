#include <radray/runtime/shader_asset.h>

#include <limits>
#include <utility>

#include <fmt/format.h>

namespace radray {
namespace {

AssetLoadTask LoadShaderAssetTask(AssetId expectedId, std::filesystem::path path) {
    std::optional<render::ShaderBinary> binary = render::ReadShaderBinary(path);
    if (!binary.has_value()) {
        co_return AssetLoadResult::Failure(fmt::format("failed to read shader binary '{}'", path.string()));
    }
    if (binary->Asset.AssetId != expectedId) {
        co_return AssetLoadResult::Failure(fmt::format(
            "shader binary '{}' contains AssetId {}, expected {}",
            path.string(),
            binary->Asset.AssetId,
            expectedId));
    }
    co_return AssetLoadResult::Success(make_unique<ShaderAsset>(std::move(*binary)));
}

}  // namespace

ShaderAsset::ShaderAsset() noexcept = default;

render::CompareFunction ToRenderCompareFunction(render::CompareFunction value) noexcept {
    switch (value) {
        case render::CompareFunction::Never: return render::CompareFunction::Never;
        case render::CompareFunction::Less: return render::CompareFunction::Less;
        case render::CompareFunction::Equal: return render::CompareFunction::Equal;
        case render::CompareFunction::LessEqual: return render::CompareFunction::LessEqual;
        case render::CompareFunction::Greater: return render::CompareFunction::Greater;
        case render::CompareFunction::NotEqual: return render::CompareFunction::NotEqual;
        case render::CompareFunction::GreaterEqual: return render::CompareFunction::GreaterEqual;
        case render::CompareFunction::Always: return render::CompareFunction::Always;
    }
    return render::CompareFunction::Always;
}

render::CullMode ToRenderCullMode(render::CullMode value) noexcept {
    switch (value) {
        case render::CullMode::Front: return render::CullMode::Front;
        case render::CullMode::Back: return render::CullMode::Back;
        case render::CullMode::None: return render::CullMode::None;
    }
    return render::CullMode::Back;
}

render::StencilOperation ToRenderStencilOperation(render::StencilOperation value) noexcept {
    switch (value) {
        case render::StencilOperation::Keep: return render::StencilOperation::Keep;
        case render::StencilOperation::Zero: return render::StencilOperation::Zero;
        case render::StencilOperation::Replace: return render::StencilOperation::Replace;
        case render::StencilOperation::Invert: return render::StencilOperation::Invert;
        case render::StencilOperation::IncrementClamp: return render::StencilOperation::IncrementClamp;
        case render::StencilOperation::DecrementClamp: return render::StencilOperation::DecrementClamp;
        case render::StencilOperation::IncrementWrap: return render::StencilOperation::IncrementWrap;
        case render::StencilOperation::DecrementWrap: return render::StencilOperation::DecrementWrap;
    }
    return render::StencilOperation::Keep;
}

render::BlendFactor ToRenderBlendFactor(render::BlendFactor value) noexcept {
    switch (value) {
        case render::BlendFactor::Zero: return render::BlendFactor::Zero;
        case render::BlendFactor::One: return render::BlendFactor::One;
        case render::BlendFactor::Src: return render::BlendFactor::Src;
        case render::BlendFactor::OneMinusSrc: return render::BlendFactor::OneMinusSrc;
        case render::BlendFactor::SrcAlpha: return render::BlendFactor::SrcAlpha;
        case render::BlendFactor::OneMinusSrcAlpha: return render::BlendFactor::OneMinusSrcAlpha;
        case render::BlendFactor::Dst: return render::BlendFactor::Dst;
        case render::BlendFactor::OneMinusDst: return render::BlendFactor::OneMinusDst;
        case render::BlendFactor::DstAlpha: return render::BlendFactor::DstAlpha;
        case render::BlendFactor::OneMinusDstAlpha: return render::BlendFactor::OneMinusDstAlpha;
        case render::BlendFactor::SrcAlphaSaturated: return render::BlendFactor::SrcAlphaSaturated;
        case render::BlendFactor::Constant: return render::BlendFactor::Constant;
        case render::BlendFactor::OneMinusConstant: return render::BlendFactor::OneMinusConstant;
        case render::BlendFactor::Src1: return render::BlendFactor::Src1;
        case render::BlendFactor::OneMinusSrc1: return render::BlendFactor::OneMinusSrc1;
        case render::BlendFactor::Src1Alpha: return render::BlendFactor::Src1Alpha;
        case render::BlendFactor::OneMinusSrc1Alpha: return render::BlendFactor::OneMinusSrc1Alpha;
    }
    return render::BlendFactor::One;
}

render::BlendOperation ToRenderBlendOperation(render::BlendOperation value) noexcept {
    switch (value) {
        case render::BlendOperation::Add: return render::BlendOperation::Add;
        case render::BlendOperation::Subtract: return render::BlendOperation::Subtract;
        case render::BlendOperation::ReverseSubtract: return render::BlendOperation::ReverseSubtract;
        case render::BlendOperation::Min: return render::BlendOperation::Min;
        case render::BlendOperation::Max: return render::BlendOperation::Max;
    }
    return render::BlendOperation::Add;
}

render::ColorWrites ToRenderColorWrites(render::ColorWrites value) noexcept {
    return render::ColorWrites{value.value()};
}

render::BlendComponent ToRenderBlendComponent(const render::BlendComponent& value) noexcept {
    return render::BlendComponent{
        .Src = ToRenderBlendFactor(value.Src),
        .Dst = ToRenderBlendFactor(value.Dst),
        .Op = ToRenderBlendOperation(value.Op)};
}

render::BlendState ToRenderBlendState(const render::BlendState& value) noexcept {
    return render::BlendState{
        .Color = ToRenderBlendComponent(value.Color),
        .Alpha = ToRenderBlendComponent(value.Alpha)};
}

render::StencilFaceState ToRenderStencilFaceState(const render::StencilFaceState& value) noexcept {
    return render::StencilFaceState{
        .Compare = ToRenderCompareFunction(value.Compare),
        .FailOp = ToRenderStencilOperation(value.FailOp),
        .DepthFailOp = ToRenderStencilOperation(value.DepthFailOp),
        .PassOp = ToRenderStencilOperation(value.PassOp)};
}

render::StencilState ToRenderStencilState(const render::StencilState& value) noexcept {
    return render::StencilState{
        .Front = ToRenderStencilFaceState(value.Front),
        .Back = ToRenderStencilFaceState(value.Back),
        .ReadMask = value.ReadMask,
        .WriteMask = value.WriteMask};
}

ShaderAsset::ShaderAsset(vector<ShaderPassDesc> passes) noexcept {
    _binary.Asset.Passes = std::move(passes);
    _valid = render::IsShaderAssetDataValid(_binary.Asset, false);
}

ShaderAsset::ShaderAsset(render::ShaderBinary binary) noexcept
    : _binary(std::move(binary)) {
    _valid = _binary.IsValid();
}

ShaderAsset::~ShaderAsset() noexcept = default;

void ShaderAsset::OnUnload(IRenderResourceRecycler& /*recycler*/) {
    _binary = {};
    _valid = false;
}

AssetTypeId ShaderAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<ShaderAsset>;
}

bool ShaderAsset::IsValid() const noexcept {
    return _valid;
}

Nullable<const render::ShaderStageArtifact*> ShaderAsset::FindCompiledStage(
    render::ShaderTarget target,
    uint32_t passIndex,
    render::ShaderStage stage,
    const vector<string>& defines) const noexcept {
    return _binary.FindStageArtifact(target, passIndex, stage, defines);
}

Nullable<const render::ShaderProgramVariantArtifact*> ShaderAsset::FindProgramVariant(
    render::ShaderTarget target,
    uint32_t passIndex,
    const vector<string>& defines) const noexcept {
    return _binary.FindProgramVariant(target, passIndex, defines);
}

std::optional<uint32_t> ShaderAsset::FindPassByName(std::string_view name) const noexcept {
    const vector<ShaderPassDesc>& passes = GetPasses();
    if (passes.size() > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    for (size_t i = 0; i < passes.size(); ++i) {
        if (passes[i].Name == name) return static_cast<uint32_t>(i);
    }
    return std::nullopt;
}

std::optional<uint32_t> ShaderAsset::FindPassByTag(std::string_view name, std::string_view value) const noexcept {
    const vector<ShaderPassDesc>& passes = GetPasses();
    if (passes.size() > std::numeric_limits<uint32_t>::max()) return std::nullopt;
    for (size_t i = 0; i < passes.size(); ++i) {
        for (const ShaderTagDesc& tag : passes[i].Tags) {
            if (tag.Name == name && tag.Value == value) return static_cast<uint32_t>(i);
        }
    }
    return std::nullopt;
}

StreamingAssetRef<ShaderAsset> LoadShaderAsset(
    AssetManager& assetManager,
    const AssetId& assetId,
    const std::filesystem::path& path) {
    return assetManager.Load<ShaderAsset>(AssetLoadRequest{
        .Id = assetId,
        .Task = LoadShaderAssetTask(assetId, path),
        .DebugName = path.string()});
}

}  // namespace radray
