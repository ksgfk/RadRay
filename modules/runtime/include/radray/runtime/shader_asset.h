#pragma once

#include <optional>
#include <string_view>

#include <radray/coroutine.h>
#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/gpu/types.h>

#include <filesystem>
#include <radray/shader/shader_layout.h>
#include <radray/runtime/asset_manager.h>

namespace radray {

struct ShaderBindingLocation {
    shader::ShaderBindingReference Reference{};
    gpu::BindingKind Kind{gpu::BindingKind::CBuffer};
    uint32_t Count{0};
    std::optional<uint32_t> TypeIndex;
    uint32_t BufferByteSize{0};
    bool ImmutableSampler{false};
    bool ActiveOnDevice{false};
};

struct ShaderPushConstantLocation {
    uint32_t PushId{0};
    uint32_t Size{0};
    std::optional<uint32_t> TypeIndex;
    bool ActiveOnDevice{false};
};

struct ShaderStageBytecode {
    uint32_t VariantIndex{0};
    gpu::RenderBackend Backend{gpu::RenderBackend::D3D12};
    gpu::ShaderStage Stage{gpu::ShaderStage::UNKNOWN};
    string Entry;
    vector<byte> Bytecode;
};

struct ShaderAssetDesc {
    shader::ShaderFamilyLayoutDesc Family;
    vector<ShaderStageBytecode> Stages;
};

class ShaderAsset final {
public:
    static std::optional<ShaderAsset> Create(gpu::Device& device, const ShaderAssetDesc& desc);
    static std::optional<ShaderAsset> Load(gpu::Device& device, const std::filesystem::path& indexPath);
    ShaderAsset(ShaderAsset&&) noexcept = default;
    ShaderAsset& operator=(ShaderAsset&&) noexcept = default;
    ShaderAsset(const ShaderAsset&) = delete;
    ShaderAsset& operator=(const ShaderAsset&) = delete;

    const shader::ShaderPipelineLayoutDesc& GetLayoutDesc() const noexcept;
    const shader::ShaderFamilyLayoutDesc& GetFamilyDesc() const noexcept;
    gpu::RenderBackend GetBackend() const noexcept;
    Nullable<const shader::ShaderVariantTargetInterfaceDesc*> GetVariantInterface(uint32_t variant) const noexcept;
    std::optional<gpu::ShaderEntry> GetStage(uint32_t variant, gpu::ShaderStage stage) const noexcept;
    std::optional<ShaderBindingLocation> FindBinding(std::string_view name) const noexcept;
    std::optional<ShaderPushConstantLocation> FindPushConstant(std::string_view name) const noexcept;
    std::optional<shader::ShaderResolvedMember> FindBufferMember(std::string_view block, std::string_view path) const;

private:
    ShaderAsset() = default;
    struct StageObject {
        uint32_t VariantIndex;
        gpu::ShaderStage Stage;
        string Entry;
        unique_ptr<gpu::Shader> Object;
    };
    shader::ShaderFamilyLayoutDesc _family;
    gpu::RenderBackend _backend{gpu::RenderBackend::D3D12};
    vector<StageObject> _stages;
};

class ShaderAssetLoader final : public AssetLoader<ShaderAsset> {
public:
    explicit ShaderAssetLoader(gpu::Device& device, Nullable<const ShaderAsset*> fallback = nullptr) noexcept;
    task<std::optional<ShaderAsset>> LoadAsync(AssetLoadContext& context) override;
    Nullable<const ShaderAsset*> GetFallback() const noexcept override;

private:
    gpu::Device& _device;
    Nullable<const ShaderAsset*> _fallback;
};

}  // namespace radray
