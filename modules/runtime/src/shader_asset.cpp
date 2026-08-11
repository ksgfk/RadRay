#include <radray/runtime/shader_asset.h>

#include <radray/file.h>

namespace radray {

ShaderAsset::ShaderAsset(
    string sourceName,
    shader::ShaderTarget target,
    ShaderJitArtifact artifact) noexcept
    : _sourceName(std::move(sourceName)),
      _target(target),
      _artifact(std::move(artifact)) {
}

ShaderAsset::~ShaderAsset() noexcept = default;

void ShaderAsset::OnUnload(AssetManager& manager) {
    // metadata 与 bytecode 都是 CPU-owned value，析构时直接释放即可。
    (void)manager;
}

RuntimeTypeId ShaderAsset::GetTypeId() const noexcept {
    return runtime_type_id_v<ShaderAsset>;
}

task<AssetLoadResult> LoadShaderAssetBundle(AssetManager& manager, BundleAssetLoadData data) {
    const auto* descriptor = dynamic_cast<const ShaderAssetDescriptor*>(data.Entry.Descriptor.get());
    if (descriptor == nullptr || !data.Entry.Locator.has_value()) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::InvalidDescriptor,
            "ShaderAsset Bundle descriptor is missing representation or locator");
    }

    if (descriptor->Representation == ShaderAssetRepresentation::AotArtifact) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::CapabilityUnavailable,
            "ShaderAsset AOT artifacts are not available in this runtime build");
    }

    Nullable<ShaderJit*> jit = manager.GetShaderJit();
    if (!jit || !jit->IsAvailable()) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::CapabilityUnavailable,
            "ShaderAsset JIT service is unavailable");
    }

    const string sourceName = data.Entry.Locator->GetValue();
    const std::filesystem::path sourcePath = data.Root / std::filesystem::path{sourceName};
    std::optional<vector<byte>> source = ReadBinaryFile(sourcePath);
    if (!source.has_value()) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::PayloadFailure,
            "ShaderAsset source file could not be read");
    }

    const std::optional<shader::ContractHash> contract =
        jit->DiscoverContractHash(sourceName, *source, descriptor->Target);
    if (!contract.has_value()) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::PayloadFailure,
            "ShaderAsset source contract discovery failed");
    }

    shader::CompileVariantRequest request{
        .SourceName = sourceName,
        .RootSource = std::move(*source),
        .Defines = {},
        .Assignments = {},
        .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(descriptor->Target)),
        .Policy = {},
        .ExpectedContract = *contract};
    std::optional<ShaderJitArtifact> artifact = jit->Compile(request, descriptor->Target);
    if (!artifact.has_value()) {
        co_return AssetLoadResult::Failure(
            AssetLoadErrorCode::PayloadFailure,
            "ShaderAsset JIT compilation failed");
    }

    co_return AssetLoadResult::Success(
        make_unique<ShaderAsset>(sourceName, descriptor->Target, std::move(*artifact)));
}

}  // namespace radray
