#pragma once

#include <stdexcept>
#include <string_view>
#include <variant>

#include <radray/render/gpu_resource.h>
#include <radray/render/pipeline_layout.h>

namespace radray::render {

class ResourceBinderException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ResourceBinder {
public:
    ResourceBinder(Device* device, RootSignature* rootSig, const PipelineLayout& layout);
    ResourceBinder(Device* device, RootSignature* rootSig, const PipelineLayoutPlan& plan, StructuredBufferStorage cbStorage);
    ResourceBinder(const ResourceBinder&) = delete;
    ResourceBinder& operator=(const ResourceBinder&) = delete;
    ResourceBinder(ResourceBinder&&) noexcept = default;
    ResourceBinder& operator=(ResourceBinder&&) noexcept = default;
    ~ResourceBinder() noexcept = default;

    std::optional<uint32_t> GetParameterId(std::string_view name) const noexcept;
    bool SetResource(uint32_t id, ResourceView* view, uint32_t arrayIndex = 0) noexcept;
    bool SetResource(std::string_view name, ResourceView* view, uint32_t arrayIndex = 0) noexcept;

    StructuredBufferView GetCBufferStorage(std::string_view name) noexcept { return _cbStorage.GetVar(name); }
    StructuredBufferReadOnlyView GetCBufferStorage(std::string_view name) const noexcept { return _cbStorage.GetVar(name); }
    StructuredBufferView GetCBufferStorage(uint32_t id) noexcept;
    StructuredBufferReadOnlyView GetCBufferStorage(uint32_t id) const noexcept;

    bool FlushConstantsTransfers(Device* device, CBufferArena& arena);
    void Bind(CommandEncoder* encoder);
    void ResetState();

    // Compatibility helpers during migration.
    std::optional<uint32_t> GetBindingId(std::string_view name) const noexcept { return GetParameterId(name); }
    StructuredBufferView GetCBuffer(std::string_view name) noexcept { return GetCBufferStorage(name); }
    StructuredBufferReadOnlyView GetCBuffer(std::string_view name) const noexcept { return GetCBufferStorage(name); }
    StructuredBufferView GetCBuffer(uint32_t id) noexcept { return GetCBufferStorage(id); }
    StructuredBufferReadOnlyView GetCBuffer(uint32_t id) const noexcept { return GetCBufferStorage(id); }
    bool Upload(Device* device, CBufferArena& arena) { return FlushConstantsTransfers(device, arena); }
    void Clear() { ResetState(); }

private:
    struct BindingRuntime {
        ShaderParameter Parameter{};
        ParameterLocation Location{};
        StructuredBufferId CBufferId{StructuredBufferStorage::InvalidId};
        bool IsStaticSampler{false};
    };

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
    vector<BindingRuntime> _bindings;
    vector<unique_ptr<BufferView>> _ownedCBufferViews;
    ParameterNameToIdMap _nameToParameterId;
};

}  // namespace radray::render
