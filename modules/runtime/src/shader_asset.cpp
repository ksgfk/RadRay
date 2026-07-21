#include <radray/runtime/shader_asset.h>

#include <limits>
#include <utility>

#include <fmt/format.h>

namespace radray {
namespace {

AssetLoadTask LoadShaderAssetTask(AssetId expectedId, std::filesystem::path path) {
    std::optional<shader::ShaderBinary> binary = shader::ReadShaderBinary(path);
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

render::CompareFunction ToRenderCompareFunction(shader::CompareFunction value) noexcept {
    switch (value) {
        case shader::CompareFunction::Never: return render::CompareFunction::Never;
        case shader::CompareFunction::Less: return render::CompareFunction::Less;
        case shader::CompareFunction::Equal: return render::CompareFunction::Equal;
        case shader::CompareFunction::LessEqual: return render::CompareFunction::LessEqual;
        case shader::CompareFunction::Greater: return render::CompareFunction::Greater;
        case shader::CompareFunction::NotEqual: return render::CompareFunction::NotEqual;
        case shader::CompareFunction::GreaterEqual: return render::CompareFunction::GreaterEqual;
        case shader::CompareFunction::Always: return render::CompareFunction::Always;
    }
    return render::CompareFunction::Always;
}

render::CullMode ToRenderCullMode(shader::CullMode value) noexcept {
    switch (value) {
        case shader::CullMode::Front: return render::CullMode::Front;
        case shader::CullMode::Back: return render::CullMode::Back;
        case shader::CullMode::None: return render::CullMode::None;
    }
    return render::CullMode::Back;
}

render::StencilOperation ToRenderStencilOperation(shader::StencilOperation value) noexcept {
    switch (value) {
        case shader::StencilOperation::Keep: return render::StencilOperation::Keep;
        case shader::StencilOperation::Zero: return render::StencilOperation::Zero;
        case shader::StencilOperation::Replace: return render::StencilOperation::Replace;
        case shader::StencilOperation::Invert: return render::StencilOperation::Invert;
        case shader::StencilOperation::IncrementClamp: return render::StencilOperation::IncrementClamp;
        case shader::StencilOperation::DecrementClamp: return render::StencilOperation::DecrementClamp;
        case shader::StencilOperation::IncrementWrap: return render::StencilOperation::IncrementWrap;
        case shader::StencilOperation::DecrementWrap: return render::StencilOperation::DecrementWrap;
    }
    return render::StencilOperation::Keep;
}

render::BlendFactor ToRenderBlendFactor(shader::BlendFactor value) noexcept {
    switch (value) {
        case shader::BlendFactor::Zero: return render::BlendFactor::Zero;
        case shader::BlendFactor::One: return render::BlendFactor::One;
        case shader::BlendFactor::Src: return render::BlendFactor::Src;
        case shader::BlendFactor::OneMinusSrc: return render::BlendFactor::OneMinusSrc;
        case shader::BlendFactor::SrcAlpha: return render::BlendFactor::SrcAlpha;
        case shader::BlendFactor::OneMinusSrcAlpha: return render::BlendFactor::OneMinusSrcAlpha;
        case shader::BlendFactor::Dst: return render::BlendFactor::Dst;
        case shader::BlendFactor::OneMinusDst: return render::BlendFactor::OneMinusDst;
        case shader::BlendFactor::DstAlpha: return render::BlendFactor::DstAlpha;
        case shader::BlendFactor::OneMinusDstAlpha: return render::BlendFactor::OneMinusDstAlpha;
        case shader::BlendFactor::SrcAlphaSaturated: return render::BlendFactor::SrcAlphaSaturated;
        case shader::BlendFactor::Constant: return render::BlendFactor::Constant;
        case shader::BlendFactor::OneMinusConstant: return render::BlendFactor::OneMinusConstant;
        case shader::BlendFactor::Src1: return render::BlendFactor::Src1;
        case shader::BlendFactor::OneMinusSrc1: return render::BlendFactor::OneMinusSrc1;
        case shader::BlendFactor::Src1Alpha: return render::BlendFactor::Src1Alpha;
        case shader::BlendFactor::OneMinusSrc1Alpha: return render::BlendFactor::OneMinusSrc1Alpha;
    }
    return render::BlendFactor::One;
}

render::BlendOperation ToRenderBlendOperation(shader::BlendOperation value) noexcept {
    switch (value) {
        case shader::BlendOperation::Add: return render::BlendOperation::Add;
        case shader::BlendOperation::Subtract: return render::BlendOperation::Subtract;
        case shader::BlendOperation::ReverseSubtract: return render::BlendOperation::ReverseSubtract;
        case shader::BlendOperation::Min: return render::BlendOperation::Min;
        case shader::BlendOperation::Max: return render::BlendOperation::Max;
    }
    return render::BlendOperation::Add;
}

render::ColorWrites ToRenderColorWrites(shader::ColorWrites value) noexcept {
    return render::ColorWrites{value.value()};
}

render::BlendComponent ToRenderBlendComponent(const shader::BlendComponent& value) noexcept {
    return render::BlendComponent{
        .Src = ToRenderBlendFactor(value.Src),
        .Dst = ToRenderBlendFactor(value.Dst),
        .Op = ToRenderBlendOperation(value.Op)};
}

render::BlendState ToRenderBlendState(const shader::BlendState& value) noexcept {
    return render::BlendState{
        .Color = ToRenderBlendComponent(value.Color),
        .Alpha = ToRenderBlendComponent(value.Alpha)};
}

render::StencilFaceState ToRenderStencilFaceState(const shader::StencilFaceState& value) noexcept {
    return render::StencilFaceState{
        .Compare = ToRenderCompareFunction(value.Compare),
        .FailOp = ToRenderStencilOperation(value.FailOp),
        .DepthFailOp = ToRenderStencilOperation(value.DepthFailOp),
        .PassOp = ToRenderStencilOperation(value.PassOp)};
}

render::StencilState ToRenderStencilState(const shader::StencilState& value) noexcept {
    return render::StencilState{
        .Front = ToRenderStencilFaceState(value.Front),
        .Back = ToRenderStencilFaceState(value.Back),
        .ReadMask = value.ReadMask,
        .WriteMask = value.WriteMask};
}

ShaderAsset::ShaderAsset(vector<ShaderPassDesc> passes) noexcept {
    _binary.Asset.Passes = std::move(passes);
    _valid = shader::IsShaderAssetDataValid(_binary.Asset, false);
}

ShaderAsset::ShaderAsset(shader::ShaderBinary binary) noexcept
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

Nullable<const shader::ShaderStageArtifact*> ShaderAsset::FindCompiledStage(
    shader::ShaderTarget target,
    uint32_t passIndex,
    shader::ShaderStage stage,
    const vector<string>& defines) const noexcept {
    return _binary.FindStageArtifact(target, passIndex, stage, defines);
}

Nullable<const shader::ShaderProgramVariantArtifact*> ShaderAsset::FindProgramVariant(
    shader::ShaderTarget target,
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
