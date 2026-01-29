#pragma once

#include <span>
#include <string_view>
#include <unordered_map>
#include <variant>

#include <radray/render/render_utility.h>

namespace radray::render {

struct BindBridgeStaticSampler {
    string Name;
    vector<SamplerDescriptor> Samplers;
};

class BindBridgeLayout {
public:
    enum class BindingKind {
        PushConst,
        RootDescriptor,
        DescriptorSet
    };
    struct BindingEntry {
        uint32_t Id{0};
        string Name;
        BindingKind Kind{BindingKind::DescriptorSet};
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        uint32_t BindCount{0};
        uint32_t BindPoint{0};
        uint32_t Space{0};
        ShaderStages Stages{ShaderStage::UNKNOWN};
        uint32_t SetIndex{0};
        uint32_t ElementIndex{0};
        uint32_t RootIndex{0};
        uint32_t PushConstSize{0};
        vector<SamplerDescriptor> StaticSamplers;
    };

    BindBridgeLayout() noexcept = default;
    explicit BindBridgeLayout(const HlslShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers = {}) noexcept;
    explicit BindBridgeLayout(const SpirvShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers = {}) noexcept;

    RootSignatureDescriptorContainer GetDescriptor() const noexcept;
    std::span<const BindingEntry> GetBindings() const noexcept { return _bindings; }
    std::optional<uint32_t> GetBindingId(std::string_view name) const noexcept;

private:
    friend class BindBridge;

    bool BuildFromHlsl(const HlslShaderDesc& desc) noexcept;
    bool BuildFromSpirv(const SpirvShaderDesc& desc) noexcept;
    void BuildBindingIndex() noexcept;
    void ApplyStaticSamplers(std::span<const BindBridgeStaticSampler> staticSamplers) noexcept;

    StructuredBufferStorage::Builder _cbStorageBuilder;
    vector<BindingEntry> _bindings;
    unordered_map<string, uint32_t> _nameToBindingId;
};

class BindBridge {
public:
    BindBridge(Device* device, RootSignature* rootSig, const BindBridgeLayout& layout);

    std::optional<uint32_t> GetBindingId(std::string_view name) const noexcept;
    bool SetResource(uint32_t id, ResourceView* view, uint32_t arrayIndex = 0) noexcept;
    bool SetResource(std::string_view name, ResourceView* view, uint32_t arrayIndex = 0) noexcept;

    StructuredBufferView GetCBuffer(std::string_view name) noexcept { return _cbStorage.GetVar(name); }
    StructuredBufferReadOnlyView GetCBuffer(std::string_view name) const noexcept { return _cbStorage.GetVar(name); }
    StructuredBufferView GetCBuffer(uint32_t id) noexcept;
    StructuredBufferReadOnlyView GetCBuffer(uint32_t id) const noexcept;

    bool Upload(Device& device, CBufferArena& arena) noexcept;

    void Bind(CommandEncoder* encoder) const noexcept;

private:
    struct PushConstBinding {
        StructuredBufferId CBufferId{StructuredBufferStorage::InvalidId};
        uint32_t Size{0};
    };

    struct RootDescriptorBinding {
        uint32_t RootIndex{0};
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        uint32_t BindCount{0};
        StructuredBufferId CBufferId{StructuredBufferStorage::InvalidId};
    };

    struct DescriptorSetBindingInfo {
        uint32_t SetIndex{0};
        uint32_t ElementIndex{0};
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        uint32_t BindCount{0};
        StructuredBufferId CBufferId{StructuredBufferStorage::InvalidId};
    };

    using BindingLocator = std::variant<PushConstBinding, RootDescriptorBinding, DescriptorSetBindingInfo>;

    struct DescSetBinding {
        uint32_t Slot{0};
        uint32_t Count{0};
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        StructuredBufferId CBufferId{StructuredBufferStorage::InvalidId};
        vector<ResourceView*> Views{};
    };

    struct DescSetRecord {
        DescriptorSet* Set{nullptr};
        vector<DescSetBinding> Bindings{};
    };

    void SetRootDescriptor(uint32_t slot, ResourceView* view) noexcept;
    void SetDescriptorSet(uint32_t setIndex, DescriptorSet* set) noexcept;
    void SetDescriptorSetResource(uint32_t setIndex, uint32_t elementIndex, uint32_t arrayIndex, ResourceView* view) noexcept;

    vector<ResourceView*> _rootDescViews;
    vector<DescSetRecord> _descSets;
    StructuredBufferStorage _cbStorage;
    vector<BindingLocator> _bindings;
    unordered_map<string, uint32_t> _nameToBindingId;
    vector<unique_ptr<DescriptorSet>> _ownedDescriptorSets{};
    vector<unique_ptr<BufferView>> _ownedCBufferViews{};
};

}  // namespace radray::render
