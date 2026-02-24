#pragma once

#include <span>
#include <string_view>
#include <variant>
#include <stdexcept>

#include <radray/hash.h>
#include <radray/structured_buffer.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/msl.h>
#include <radray/render/gpu_resource.h>

namespace radray::render {

using BindBridgeNameToIdMap = unordered_map<string, uint32_t, StringHash, StringEqual>;

class RootSignatureDescriptorContainer {
public:
    RootSignatureDescriptorContainer() noexcept = default;
    explicit RootSignatureDescriptorContainer(const RootSignatureDescriptor& desc) noexcept;

    const RootSignatureDescriptor& Get() const noexcept { return _desc; }

public:
    RootSignatureDescriptor _desc{};
    vector<RootSignatureRootDescriptor> _rootDescriptors;
    vector<RootSignatureSetElement> _elements;
    vector<RootSignatureStaticSampler> _staticSamplers;
    vector<RootSignatureDescriptorSet> _descriptorSets;
};

struct BindBridgeStaticSampler {
    string Name;
    vector<SamplerDescriptor> Samplers;
};

struct BindBridgePushConstEntry {
    string Name;
    uint32_t Id{0};
    uint32_t BindPoint{0};
    uint32_t Space{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    uint32_t Size{0};
};

struct BindBridgeRootDescriptorEntry {
    string Name;
    uint32_t Id{0};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t BindPoint{0};
    uint32_t Space{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    uint32_t RootIndex{0};
};

struct BindBridgeDescriptorSetEntry {
    string Name;
    uint32_t Id{0};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t BindCount{0};
    uint32_t BindPoint{0};
    uint32_t Space{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    uint32_t SetIndex{0};
    uint32_t ElementIndex{0};
    bool IsStaticSampler{false};
    vector<SamplerDescriptor> StaticSamplerDescs;
};

using BindBridgeBindingEntry = std::variant<BindBridgePushConstEntry, BindBridgeRootDescriptorEntry, BindBridgeDescriptorSetEntry>;

struct BindBridgeIRBinding {
    enum class Placement : uint8_t {
        PushConstant,
        RootDescriptor,
        DescriptorSet,
    };

    string Name;
    Placement PlacementType{Placement::DescriptorSet};
    ResourceBindType Type{ResourceBindType::UNKNOWN};
    uint32_t BindCount{0};
    uint32_t BindPoint{0};
    uint32_t Space{0};
    ShaderStages Stages{ShaderStage::UNKNOWN};
    uint32_t Size{0};  // Byte size for constants/cbuffers when known
    bool IsBindless{false};
    uint32_t RootIndex{0};
    uint32_t SetIndex{0};
    uint32_t ElementIndex{0};
};

struct BindBridgeIR {
    vector<BindBridgeIRBinding> Bindings;
};

struct BindBridgePlannedLayout {
    vector<BindBridgeBindingEntry> Bindings;
    RootSignatureDescriptorContainer Descriptor;
    BindBridgeNameToIdMap NameToBindingId;
};

class IBindBridgeLayoutPlanner {
public:
    virtual ~IBindBridgeLayoutPlanner() noexcept = default;
    virtual std::optional<BindBridgePlannedLayout> Plan(const BindBridgeIR& ir, std::span<const BindBridgeStaticSampler> staticSamplers) const noexcept = 0;
};

class D3D12BindBridgeLayoutPlanner final : public IBindBridgeLayoutPlanner {
public:
    std::optional<BindBridgePlannedLayout> Plan(const BindBridgeIR& ir, std::span<const BindBridgeStaticSampler> staticSamplers) const noexcept override;
};

class VulkanBindBridgeLayoutPlanner final : public IBindBridgeLayoutPlanner {
public:
    std::optional<BindBridgePlannedLayout> Plan(const BindBridgeIR& ir, std::span<const BindBridgeStaticSampler> staticSamplers) const noexcept override;
};

class MetalBindBridgeLayoutPlanner final : public IBindBridgeLayoutPlanner {
public:
    std::optional<BindBridgePlannedLayout> Plan(const BindBridgeIR& ir, std::span<const BindBridgeStaticSampler> staticSamplers) const noexcept override;
};

class BindBridgeLayout {
public:
    using PushConstEntry = BindBridgePushConstEntry;
    using RootDescriptorEntry = BindBridgeRootDescriptorEntry;
    using DescriptorSetEntry = BindBridgeDescriptorSetEntry;
    using BindingEntry = BindBridgeBindingEntry;

    BindBridgeLayout() noexcept = default;
    explicit BindBridgeLayout(const HlslShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers = {}) noexcept;
    explicit BindBridgeLayout(const SpirvShaderDesc& desc, std::span<const BindBridgeStaticSampler> staticSamplers = {}) noexcept;
    explicit BindBridgeLayout(const MslShaderReflection& desc, std::span<const BindBridgeStaticSampler> staticSamplers = {}) noexcept;
    explicit BindBridgeLayout(const BindBridgeIR& ir, const IBindBridgeLayoutPlanner& planner, std::span<const BindBridgeStaticSampler> staticSamplers = {}) noexcept;

    RootSignatureDescriptorContainer GetDescriptor() const noexcept;
    std::span<const BindingEntry> GetBindings() const noexcept { return _bindings; }
    std::optional<uint32_t> GetBindingId(std::string_view name) const noexcept;

    static std::optional<BindBridgeIR> BuildIRFromHlsl(const HlslShaderDesc& desc) noexcept;
    static std::optional<BindBridgeIR> BuildIRFromSpirv(const SpirvShaderDesc& desc) noexcept;
    static std::optional<BindBridgeIR> BuildIRFromMsl(const MslShaderReflection& desc) noexcept;
    static std::optional<BindBridgePlannedLayout> PlanLayout(const BindBridgeIR& ir, const IBindBridgeLayoutPlanner& planner, std::span<const BindBridgeStaticSampler> staticSamplers = {}) noexcept;

    static std::optional<vector<BindingEntry>> BuildFromHlsl(const HlslShaderDesc& desc) noexcept;
    static std::optional<vector<BindingEntry>> BuildFromSpirv(const SpirvShaderDesc& desc) noexcept;
    static std::optional<vector<BindingEntry>> BuildFromMsl(const MslShaderReflection& desc) noexcept;
    static std::optional<StructuredBufferStorage::Builder> CreateCBufferStorageBuilder(const HlslShaderDesc& desc) noexcept;
    static std::optional<StructuredBufferStorage::Builder> CreateCBufferStorageBuilder(const SpirvShaderDesc& desc) noexcept;
    static std::optional<StructuredBufferStorage::Builder> CreateCBufferStorageBuilder(const MslShaderReflection& desc) noexcept;
    static RootSignatureDescriptorContainer BuildDescriptor(std::span<const BindingEntry> bindings) noexcept;
    static BindBridgeNameToIdMap BuildBindingIndex(vector<BindingEntry>& bindings) noexcept;
    static void ApplyStaticSamplers(vector<BindingEntry>& bindings, std::span<const BindBridgeStaticSampler> staticSamplers) noexcept;

private:
    friend class BindBridge;

    void SetPlannedLayout(BindBridgePlannedLayout&& planned) noexcept;

    vector<BindingEntry> _bindings;
    RootSignatureDescriptorContainer _descriptor;
    BindBridgeNameToIdMap _nameToBindingId;
    StructuredBufferStorage::Builder _cbStorageBuilder;
};

class BindBridgeException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class BindBridge {
public:
    BindBridge(Device* device, RootSignature* rootSig, const BindBridgeLayout& layout);
    BindBridge(const BindBridge&) = delete;
    BindBridge& operator=(const BindBridge&) = delete;
    BindBridge(BindBridge&&) noexcept = default;
    BindBridge& operator=(BindBridge&&) noexcept = default;
    ~BindBridge() noexcept = default;

    std::optional<uint32_t> GetBindingId(std::string_view name) const noexcept;
    bool SetResource(uint32_t id, ResourceView* view, uint32_t arrayIndex = 0) noexcept;
    bool SetResource(std::string_view name, ResourceView* view, uint32_t arrayIndex = 0) noexcept;

    StructuredBufferView GetCBuffer(std::string_view name) noexcept { return _cbStorage.GetVar(name); }
    StructuredBufferReadOnlyView GetCBuffer(std::string_view name) const noexcept { return _cbStorage.GetVar(name); }
    StructuredBufferView GetCBuffer(uint32_t id) noexcept;
    StructuredBufferReadOnlyView GetCBuffer(uint32_t id) const noexcept;

    void Bind(CommandEncoder* encoder);

    bool Upload(Device* device, CBufferArena& arena);

    void Clear();

private:
    struct PushConstBinding {
        StructuredBufferId CBufferId{StructuredBufferStorage::InvalidId};
        uint32_t Size{0};
    };

    struct RootDescriptorBinding {
        uint32_t RootIndex{0};
        ResourceBindType Type{ResourceBindType::UNKNOWN};
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
        vector<uint8_t> Dirty{};
    };

    struct DescSetRecord {
        vector<DescSetBinding> Bindings{};
        unique_ptr<DescriptorSet> OwnedSet{};
    };

    struct RootDescriptorView {
        Buffer* Buffer{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};
    };

    void SetRootDescriptor(uint32_t slot, Buffer* buffer, uint64_t offset, uint64_t size) noexcept;
    void SetDescriptorSetResource(uint32_t setIndex, uint32_t elementIndex, uint32_t arrayIndex, ResourceView* view) noexcept;

    vector<RootDescriptorView> _rootDescViews;
    unordered_map<uint32_t, DescSetRecord> _descSets;
    StructuredBufferStorage _cbStorage;
    vector<BindingLocator> _bindings;
    vector<unique_ptr<BufferView>> _ownedCBufferViews;
    BindBridgeNameToIdMap _nameToBindingId;
};

}  // namespace radray::render
