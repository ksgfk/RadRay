#pragma once

#include <radray/runtime/asset_bundle_descriptors.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_jit.h>

namespace radray {

/// JIT 编译结果的 CPU 侧资产。它只持有编译器返回的 metadata；后续 RHI pipeline 创建
/// 由 render 层消费该 artifact，不把 Bundle 或 shaderlib 路径带进资产对象。
class ShaderAsset final : public Asset {
public:
    ShaderAsset(
        string sourceName,
        shader::ShaderTarget target,
        ShaderJitArtifact artifact) noexcept;
    ~ShaderAsset() noexcept override;

    void OnUnload(AssetManager& manager) override;
    RuntimeTypeId GetTypeId() const noexcept override;

    const string& GetSourceName() const noexcept { return _sourceName; }
    shader::ShaderTarget GetTarget() const noexcept { return _target; }
    const ShaderJitArtifact& GetArtifact() const noexcept { return _artifact; }

private:
    string _sourceName;
    shader::ShaderTarget _target{shader::ShaderTarget::DXIL};
    ShaderJitArtifact _artifact;
};

/// 由 AssetManager 的安全 Bundle 快照调用。AOT representation 在当前 runtime 明确返回
/// CapabilityUnavailable，绝不静默回退到 JIT。
task<AssetLoadResult> LoadShaderAssetBundle(AssetManager& manager, BundleAssetLoadData data);

template <>
struct RuntimeTypeTrait<ShaderAsset> {
    static constexpr RuntimeTypeId value{0xc3cc29f0, 0x5f9c, 0x4a1a, 0x9b, 0x64, 0xa8, 0x12, 0xd3, 0x6e, 0x4f, 0x71};
    using Bases = std::tuple<Asset>;
};

}  // namespace radray
