#pragma once

#include <span>
#include <string_view>
#include <stdexcept>
#include <unordered_map>

#include <radray/allocator.h>
#include <radray/vertex_data.h>
#include <radray/structured_buffer.h>

#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/spvc.h>
#include <radray/render/gpu_resource.h>

namespace radray::render {

struct SemanticMapping {
    std::string_view Semantic{};
    uint32_t SemanticIndex{0};
    uint32_t Location{0};
    VertexFormat Format{VertexFormat::UNKNOWN};
};
std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept;

std::optional<StructuredBufferStorage::Builder> CreateCBufferStorageBuilder(const HlslShaderDesc& desc) noexcept;
std::optional<StructuredBufferStorage::Builder> CreateCBufferStorageBuilder(const SpirvShaderDesc& desc) noexcept;
std::optional<StructuredBufferStorage> CreateCBufferStorage(const HlslShaderDesc& desc) noexcept;
std::optional<StructuredBufferStorage> CreateCBufferStorage(const SpirvShaderDesc& desc) noexcept;

class RootSignatureBinder;
class RootSignatureDetail;

class RootSignatureDescriptorContainer {
public:
    const RootSignatureDescriptor& Get() const noexcept { return _desc; }

private:
    RootSignatureDescriptor _desc{};
    vector<RootSignatureRootDescriptor> _rootDescriptors;
    vector<RootSignatureSetElement> _elements;
    vector<SamplerDescriptor> _staticSamplers;
    vector<RootSignatureDescriptorSet> _descriptorSets;

    friend class RootSignatureDetail;
};

class RootSignatureDetail {
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
        uint32_t SetIndex{0};
        uint32_t ElementIndex{0};
        uint32_t RootIndex{0};
        uint32_t PushConstSize{0};
    };

    struct BindData {
        string Name;
        uint32_t BindPoint;
        uint32_t BindCount;
        uint32_t Space;
        ShaderStages Stages;
    };

    struct PushConst : BindData {
        uint32_t Size;
    };

    struct RootDesc : BindData {
        ResourceBindType Type;
    };

    struct DescSetElem : BindData {
        ResourceBindType Type;
    };

    struct DescSet {
        vector<DescSetElem> Elems;
    };

    RootSignatureDetail() noexcept = default;
    explicit RootSignatureDetail(const HlslShaderDesc& desc) noexcept;
    explicit RootSignatureDetail(const SpirvShaderDesc& desc) noexcept;
    RootSignatureDetail(
        vector<DescSet> descSets,
        vector<RootDesc> rootDescs,
        std::optional<PushConst> pushConst,
        StructuredBufferStorage::Builder builder) noexcept;

    RootSignatureDescriptorContainer ToDescriptor() const noexcept;
    RootSignatureBinder MakeBinder() const noexcept;

    const std::optional<PushConst>& GetPushConstant() const noexcept { return _pushConst; }
    std::span<const RootDesc> GetRootDescs() const noexcept { return _rootDescs; }
    std::span<const DescSet> GetDescSets() const noexcept { return _descSets; }
    std::span<const BindingEntry> GetBindings() const noexcept { return _bindings; }
    const BindingEntry* GetBinding(uint32_t id) const noexcept;
    std::optional<uint32_t> GetBindingId(std::string_view name) const noexcept;

private:
    void BuildBindingIndex() noexcept;

    vector<DescSet> _descSets;
    vector<RootDesc> _rootDescs;
    std::optional<PushConst> _pushConst;
    StructuredBufferStorage::Builder _cbStorageBuilder;
    vector<BindingEntry> _bindings;
    std::unordered_map<string, uint32_t> _nameToBindingId;
};

class RootSignatureBinder {
public:
    StructuredBufferStorage& GetCBufferStorage() noexcept { return _cbStorage; }
    const StructuredBufferStorage& GetCBufferStorage() const noexcept { return _cbStorage; }
    StructuredBufferView GetCBuffer(std::string_view name) noexcept { return _cbStorage.GetVar(name); }
    StructuredBufferReadOnlyView GetCBuffer(std::string_view name) const noexcept { return _cbStorage.GetVar(name); }

    void SetRootDescriptor(uint32_t slot, ResourceView* view) noexcept;
    void SetDescriptorSet(uint32_t setIndex, DescriptorSet* set) noexcept;
    void SetDescriptorSetResource(uint32_t setIndex, uint32_t elementIndex, uint32_t arrayIndex, ResourceView* view) noexcept;

    std::optional<uint32_t> GetBindingId(std::string_view name) const noexcept;
    bool SetResource(uint32_t id, ResourceView* view, uint32_t arrayIndex = 0) noexcept;
    bool SetResource(std::string_view name, ResourceView* view, uint32_t arrayIndex = 0) noexcept;
    StructuredBufferView GetCBuffer(uint32_t id) noexcept;
    StructuredBufferReadOnlyView GetCBuffer(uint32_t id) const noexcept;

    void Bind(CommandEncoder* encoder) const noexcept;

private:
    struct BindingLocator {
        RootSignatureDetail::BindingKind Kind{RootSignatureDetail::BindingKind::DescriptorSet};
        ResourceBindType Type{ResourceBindType::UNKNOWN};
        uint32_t BindCount{0};
        uint32_t SetIndex{0};
        uint32_t ElementIndex{0};
        uint32_t RootIndex{0};
        StructuredBufferId CBufferId{StructuredBufferStorage::InvalidId};
        uint32_t PushConstSize{0};
    };

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

    StructuredBufferId _cbPushConst{StructuredBufferStorage::InvalidId};
    uint32_t _pushConstSize{0};
    vector<StructuredBufferId> _cbRootDescs;
    vector<ResourceView*> _rootDescViews;
    vector<DescSetRecord> _descSets;
    StructuredBufferStorage _cbStorage;
    vector<BindingLocator> _bindings;
    std::unordered_map<string, uint32_t> _nameToBindingId;

    friend class RootSignatureDetail;
};

Nullable<unique_ptr<RootSignature>> CreateSerializedRootSignature(Device* device, std::span<const byte> data) noexcept;

std::optional<RootSignatureDetail> CreateRootSignatureDetail(const HlslShaderDesc& desc) noexcept;

}  // namespace radray::render
