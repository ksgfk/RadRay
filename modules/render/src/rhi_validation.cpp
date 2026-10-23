#include "rhi_validation.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <mutex>
#include <radray/basic_math.h>

#include <magic_enum/magic_enum.hpp>
#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif
#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

// Optional RHI interception and CPU contracts: docs/architecture/render-rhi.md
namespace radray::render::detail {
namespace {

struct ObjectRecord;

struct ValidationContext {
    shared_ptr<Device> NativeDevice;
    Nullable<Device*> Owner{nullptr};
    RhiValidationOptions Options;
    mutable std::mutex Mutex;
    unordered_map<const RenderBase*, ObjectRecord*> Objects;
    size_t LiveOwned{0};

    bool Reject(std::string_view operation, std::string_view reason) const noexcept {
        const auto message = fmt::format("RHI validation: {}: {}", operation, reason);
        if (Options.LogCallback)
            Options.LogCallback(LogLevel::Err, message, Options.LogUserData);
        else
            RADRAY_ERR_LOG("{}", message);
        return false;
    }

    void Fatal(std::string_view operation, std::string_view reason) const noexcept {
        Reject(operation, reason);
        RADRAY_ABORT("RHI validation: {}: {}", operation, reason);
    }

    Nullable<ObjectRecord*> Find(const RenderBase* object) const noexcept {
        std::lock_guard lock{Mutex};
        const auto found = Objects.find(object);
        return found == Objects.end() ? Nullable<ObjectRecord*>{nullptr} : found->second;
    }
};

#ifdef RADRAY_ENABLE_VULKAN
struct DescriptorCountsVulkan {
    uint64_t Samplers{0};
    uint64_t UniformBuffers{0};
    uint64_t DynamicUniformBuffers{0};
    uint64_t StorageBuffers{0};
    uint64_t DynamicStorageBuffers{0};
    uint64_t SampledImages{0};
    uint64_t StorageImages{0};
    uint64_t Resources{0};

    void Add(const BindingDescriptor& binding) noexcept {
        switch (binding.Kind) {
            case BindingKind::Sampler: Samplers += binding.Count; return;
            case BindingKind::CBuffer:
                UniformBuffers += binding.Count;
                if (binding.Vulkan->Placement == VulkanBufferDescriptorPlacement::Dynamic) DynamicUniformBuffers += binding.Count;
                break;
            case BindingKind::StructuredBuffer:
            case BindingKind::RWStructuredBuffer:
            case BindingKind::RawBuffer:
            case BindingKind::RWRawBuffer:
                StorageBuffers += binding.Count;
                if (binding.Vulkan->Placement == VulkanBufferDescriptorPlacement::Dynamic) DynamicStorageBuffers += binding.Count;
                break;
            case BindingKind::Texture:
            case BindingKind::TypedBuffer: SampledImages += binding.Count; break;
            case BindingKind::RWTexture:
            case BindingKind::RWTypedBuffer: StorageImages += binding.Count; break;
        }
        Resources += binding.Count;
    }
};

bool ValidateLayoutLimitsVulkan(const ValidationContext& context, const vulkan::DeviceVulkan& device,
                                const PipelineLayoutDescriptor& layout) noexcept {
    const auto& limits = device._properties.limits;
    DescriptorCountsVulkan total;
    array<DescriptorCountsVulkan, 3> stages{};
    const ShaderStage stageTypes[] = {ShaderStage::Vertex, ShaderStage::Pixel, ShaderStage::Compute};
    for (const auto& group : layout.Groups)
        for (const auto& binding : group.Layout->GetDesc().Bindings) {
            total.Add(binding);
            for (size_t i = 0; i < stages.size(); ++i)
                if (binding.Stages.HasFlag(stageTypes[i])) stages[i].Add(binding);
        }
    const auto fits = [&context](uint64_t count, uint32_t limit, std::string_view name) {
        return count <= limit || context.Reject("CreatePipelineLayout", fmt::format("{} exceeds {}", count, name));
    };
    if (!fits(total.Samplers, limits.maxDescriptorSetSamplers, "maxDescriptorSetSamplers") ||
        !fits(total.UniformBuffers - total.DynamicUniformBuffers, limits.maxDescriptorSetUniformBuffers, "maxDescriptorSetUniformBuffers") ||
        !fits(total.DynamicUniformBuffers, limits.maxDescriptorSetUniformBuffersDynamic, "maxDescriptorSetUniformBuffersDynamic") ||
        !fits(total.StorageBuffers - total.DynamicStorageBuffers, limits.maxDescriptorSetStorageBuffers, "maxDescriptorSetStorageBuffers") ||
        !fits(total.DynamicStorageBuffers, limits.maxDescriptorSetStorageBuffersDynamic, "maxDescriptorSetStorageBuffersDynamic") ||
        !fits(total.SampledImages, limits.maxDescriptorSetSampledImages, "maxDescriptorSetSampledImages") ||
        !fits(total.StorageImages, limits.maxDescriptorSetStorageImages, "maxDescriptorSetStorageImages")) return false;
    for (const auto& stage : stages) {
        if (!fits(stage.Samplers, limits.maxPerStageDescriptorSamplers, "maxPerStageDescriptorSamplers") ||
            !fits(stage.UniformBuffers, limits.maxPerStageDescriptorUniformBuffers, "maxPerStageDescriptorUniformBuffers") ||
            !fits(stage.StorageBuffers, limits.maxPerStageDescriptorStorageBuffers, "maxPerStageDescriptorStorageBuffers") ||
            !fits(stage.SampledImages, limits.maxPerStageDescriptorSampledImages, "maxPerStageDescriptorSampledImages") ||
            !fits(stage.StorageImages, limits.maxPerStageDescriptorStorageImages, "maxPerStageDescriptorStorageImages") ||
            !fits(stage.Resources, limits.maxPerStageResources, "maxPerStageResources")) return false;
    }
    return true;
}
#endif

struct ObjectRecord {
    shared_ptr<ValidationContext> Context;
    RenderBase* Native;
    RenderBase* Public;
    bool Owned;
    bool Live{true};
    bool Open{false};
    size_t Dependents{0};
    vector<ObjectRecord*> Dependencies;
    string Name;

    ObjectRecord(shared_ptr<ValidationContext> context, RenderBase* native, RenderBase* object, bool owned)
        : Context(std::move(context)), Native(native), Public(object), Owned(owned) {
        std::lock_guard lock{Context->Mutex};
        Context->Objects.emplace(Public, this);
        if (Owned) ++Context->LiveOwned;
    }

    virtual ~ObjectRecord() noexcept {
        Retire();
        std::lock_guard lock{Context->Mutex};
        Context->Objects.erase(Public);
    }

    bool Ready(std::string_view operation) const noexcept {
        return (Live && Context->NativeDevice->IsValid() && Native->IsValid()) ||
               Context->Reject(operation, "object is destroyed or invalid");
    }

    bool CanDestroy(std::string_view operation) const noexcept {
        if (Open) return Context->Reject(operation, "object has an open recording or encoder");
        std::lock_guard lock{Context->Mutex};
        // The caller reports after releasing the lock, including callbacks from other workers.
        return Dependents == 0;
    }

    void AddDependency(ObjectRecord* dependency) {
        if (std::find(Dependencies.begin(), Dependencies.end(), dependency) != Dependencies.end()) return;
        std::lock_guard lock{Context->Mutex};
        ++dependency->Dependents;
        Dependencies.push_back(dependency);
    }

    void RemoveDependency(ObjectRecord* dependency) noexcept {
        const auto found = std::find(Dependencies.begin(), Dependencies.end(), dependency);
        if (found == Dependencies.end()) return;
        std::lock_guard lock{Context->Mutex};
        --dependency->Dependents;
        Dependencies.erase(found);
    }

    void Retire() noexcept {
        if (!Live) return;
        std::lock_guard lock{Context->Mutex};
        for (auto* dependency : Dependencies) --dependency->Dependents;
        Dependencies.clear();
        Live = false;
        if (Owned) --Context->LiveOwned;
    }
};

template <class T>
class ValidatedObject : public T, public ObjectRecord {
public:
    ValidatedObject(shared_ptr<ValidationContext> context, unique_ptr<T> native)
        : ObjectRecord(std::move(context), native.get(), static_cast<T*>(this), true), _owned(std::move(native)) {}
    ValidatedObject(shared_ptr<ValidationContext> context, T* native)
        : ObjectRecord(std::move(context), native, static_cast<T*>(this), false) {}
    ~ValidatedObject() noexcept override {
        if (Live && !CanDestroy("destructor")) Context->Fatal("destructor", "object still has dependents or an open recording");
        if (Live && Owned) Native->Destroy();
        Retire();
    }
    bool IsValid() const noexcept override { return Live && Context->NativeDevice->IsValid() && Native->IsValid(); }
    void Destroy() noexcept override {
        if (!Live) return;
        if (!Owned) {
            Context->Reject("Destroy", "object is borrowed from its owner");
            return;
        }
        if (!CanDestroy("Destroy")) {
            Context->Reject("Destroy", "object still has dependents or an open recording");
            return;
        }
        Native->Destroy();
        Retire();
    }
    void ForwardDebugName(std::string_view name) noexcept {
        if (!Ready("SetDebugName")) return;
        Name = name;
        static_cast<IDebugName*>(static_cast<T*>(Native))->SetDebugName(name);
    }
    T* Real() const noexcept { return static_cast<T*>(Native); }
    unique_ptr<T> TakeNative() noexcept { return std::move(_owned); }

private:
    unique_ptr<T> _owned;
};

template <class T>
Nullable<T*> Lookup(const ValidationContext& context, T* object, std::string_view operation) noexcept {
    const auto record = context.Find(object);
    if (!record || !record.Get()->Ready(operation)) {
        if (!record) context.Reject(operation, "object is null, foreign, or not registered");
        return nullptr;
    }
    if (record.Get()->Public->GetTag() != object->GetTag()) {
        context.Reject(operation, "object type mismatch");
        return nullptr;
    }
    return static_cast<T*>(record.Get()->Native);
}

template <class Wrapper, class T>
Nullable<Wrapper*> WrapperOf(const ValidationContext& context, T* object, std::string_view operation) noexcept {
    if (!Lookup(context, object, operation)) return nullptr;
    const auto record = context.Find(object);
    auto* result = dynamic_cast<Wrapper*>(record.Get());
    if (!result) context.Reject(operation, "object type mismatch");
    return result;
}

template <class E>
bool ValidEnum(E value) noexcept { return magic_enum::enum_contains(value); }

bool RangeFits(uint64_t total, uint64_t offset, uint64_t size, bool allowEmpty = false) noexcept {
    return offset <= total && size <= total - offset && (allowEmpty || size != 0);
}

bool BufferRangeFits(const BufferDescriptor& desc, BufferRange range) noexcept {
    return range.Offset < desc.Size && (range.Size == BufferRange::All() || RangeFits(desc.Size, range.Offset, range.Size));
}

uint64_t RangeSize(const BufferDescriptor& desc, BufferRange range) noexcept {
    return range.Size == BufferRange::All() ? desc.Size - range.Offset : range.Size;
}

uint32_t ArrayLayers(const TextureDescriptor& desc) noexcept {
    return desc.Dim == TextureDimension::Dim3D ? 1 : desc.DepthOrArraySize;
}

bool Is3D(const TextureDescriptor& desc) noexcept { return desc.Dim == TextureDimension::Dim3D; }

class BufferValidation final : public ValidatedObject<Buffer> {
public:
    BufferValidation(shared_ptr<ValidationContext> context, unique_ptr<Buffer> native)
        : ValidatedObject(std::move(context), std::move(native)), Desc(Real()->GetDesc()) {}
    BufferDescriptor Desc;
    bool Mapped{false};
    void* Map(uint64_t offset, uint64_t size) noexcept override {
        if (!Ready("Map")) return nullptr;
        if (Mapped || Desc.Memory == MemoryType::Device || !RangeFits(Desc.Size, offset, size)) {
            Context->Reject("Map", "buffer is already mapped, not host visible, or range is invalid");
            return nullptr;
        }
        if (!Desc.Usage.HasFlag(BufferUse::MapRead) && !Desc.Usage.HasFlag(BufferUse::MapWrite)) {
            Context->Reject("Map", "buffer must declare MapRead or MapWrite usage");
            return nullptr;
        }
        auto* result = Real()->Map(offset, size);
        if (result) Mapped = true;
        return result;
    }
    void Unmap() noexcept override {
        if (!Ready("Unmap")) return;
        if (!Mapped) {
            Context->Reject("Unmap", "buffer is not mapped");
            return;
        }
        Real()->Unmap();
        Mapped = false;
    }
    bool ValidateMapped(BufferRange range, bool write, std::string_view operation) const noexcept {
        if (!Ready(operation)) return false;
        if (!Mapped && !Desc.Hints.HasFlag(ResourceHint::PersistentMap))
            return Context->Reject(operation, "buffer is not mapped");
        if (Desc.Memory == MemoryType::Device || !Desc.Usage.HasFlag(write ? BufferUse::MapWrite : BufferUse::MapRead) || !BufferRangeFits(Desc, range))
            return Context->Reject(operation, "mapped memory usage or range is invalid");
        return true;
    }
    void FlushMappedRange(BufferRange range) noexcept override {
        if (ValidateMapped(range, true, "FlushMappedRange")) Real()->FlushMappedRange(range);
    }
    void InvalidateMappedRange(BufferRange range) noexcept override {
        if (ValidateMapped(range, false, "InvalidateMappedRange")) Real()->InvalidateMappedRange(range);
    }
    BufferDescriptor GetDesc() const noexcept override {
        if (!Ready("GetDesc")) Context->Fatal("GetDesc", "cannot query a destroyed object");
        return Desc;
    }
    Device* GetDevice() const noexcept override {
        if (!Ready("GetDevice")) Context->Fatal("GetDevice", "cannot query a destroyed resource");
        return Context->Owner.Get();
    }
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

class TextureValidation final : public ValidatedObject<Texture> {
public:
    TextureValidation(shared_ptr<ValidationContext> context, unique_ptr<Texture> native)
        : ValidatedObject(std::move(context), std::move(native)), Desc(Real()->GetDesc()) {}
    TextureValidation(shared_ptr<ValidationContext> context, Texture* native)
        : ValidatedObject(std::move(context), native), Desc(Real()->GetDesc()) {}
    TextureDescriptor Desc;
    TextureDescriptor GetDesc() const noexcept override {
        if (!Ready("GetDesc")) Context->Fatal("GetDesc", "cannot query a destroyed object");
        return Desc;
    }
    Device* GetDevice() const noexcept override {
        if (!Ready("GetDevice")) Context->Fatal("GetDevice", "cannot query a destroyed resource");
        return Context->Owner.Get();
    }
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

class TextureViewValidation final : public ValidatedObject<TextureView> {
public:
    TextureViewValidation(shared_ptr<ValidationContext> context, unique_ptr<TextureView> native, Texture* texture)
        : ValidatedObject(std::move(context), std::move(native)), Desc(Real()->GetDesc()) {
        Desc.Target = texture;
        AddDependency(Context->Find(texture).Get());
    }
    TextureViewDescriptor Desc;
    TextureViewDescriptor GetDesc() const noexcept override {
        if (!Ready("GetDesc")) Context->Fatal("GetDesc", "cannot query a destroyed object");
        return Desc;
    }
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

class ShaderValidation final : public ValidatedObject<Shader> {
public:
    using ValidatedObject::ValidatedObject;
    ShaderStages GetStages() const noexcept override {
        if (!Ready("GetStages")) Context->Fatal("GetStages", "cannot query a destroyed shader");
        return Real()->GetStages();
    }
};

class SamplerValidation final : public ValidatedObject<Sampler> {
public:
    using ValidatedObject::ValidatedObject;
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

class FenceValidation final : public ValidatedObject<Fence> {
public:
    using ValidatedObject::ValidatedObject;
    uint64_t GetCompletedValue() const noexcept override {
        if (!Ready("GetCompletedValue")) Context->Fatal("GetCompletedValue", "cannot query a destroyed fence");
        return Real()->GetCompletedValue();
    }
    uint64_t GetLastSignaledValue() const noexcept override {
        if (!Ready("GetLastSignaledValue")) Context->Fatal("GetLastSignaledValue", "cannot query a destroyed fence");
        return Real()->GetLastSignaledValue();
    }
    void Wait() noexcept override {
        if (Ready("Wait")) Real()->Wait();
    }
    void Wait(uint64_t value) noexcept override {
        if (Ready("Wait")) Real()->Wait(value);
    }
    bool WaitFor(uint64_t value, uint64_t timeout) noexcept override { return Ready("WaitFor") && Real()->WaitFor(value, timeout); }
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

class QueryPoolValidation final : public ValidatedObject<QueryPool> {
public:
    QueryPoolValidation(shared_ptr<ValidationContext> context, unique_ptr<QueryPool> native)
        : ValidatedObject(std::move(context), std::move(native)), Type(Real()->GetType()), Count(Real()->GetCount()) {}
    QueryType Type;
    uint32_t Count;
    QueryType GetType() const noexcept override {
        if (!Ready("GetType")) Context->Fatal("GetType", "cannot query a destroyed pool");
        return Type;
    }
    uint32_t GetCount() const noexcept override {
        if (!Ready("GetCount")) Context->Fatal("GetCount", "cannot query a destroyed pool");
        return Count;
    }
    TimestampQueryCalibration GetTimestampCalibration(CommandQueue* queue) const noexcept override {
        if (!Ready("GetTimestampCalibration")) Context->Fatal("GetTimestampCalibration", "cannot query a destroyed pool");
        const auto native = Lookup(*Context, queue, "GetTimestampCalibration");
        if (!native) Context->Fatal("GetTimestampCalibration", "queue is null, foreign, or invalid");
        return Real()->GetTimestampCalibration(native.Get());
    }
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

struct BindingInfo {
    uint32_t Binding{0}, Count{0};
    BindingKind Kind{BindingKind::CBuffer};
    bool Dynamic{false}, Immutable{false};
};
class BindingGroupLayoutValidation final : public ValidatedObject<BindingGroupLayout> {
public:
    using ValidatedObject::ValidatedObject;
    vector<BindingDescriptor> Descriptors;
    vector<BindingInfo> Bindings;
    BindingGroupLayoutDescriptor GetDesc() const noexcept override { return {Descriptors}; }
    Nullable<const BindingInfo*> Resolve(uint32_t id, std::string_view operation) const noexcept {
        if (!Ready(operation)) return nullptr;
        const auto found = std::find_if(Bindings.begin(), Bindings.end(), [&](const auto& b) { return b.Binding == id; });
        if (found == Bindings.end()) {
            Context->Reject(operation, "unknown group binding ID");
            return nullptr;
        }
        return &*found;
    }
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};
class PipelineLayoutValidation final : public ValidatedObject<PipelineLayout> {
public:
    using ValidatedObject::ValidatedObject;
    vector<PipelineLayoutGroupDescriptor> Groups;
    vector<PushConstantDescriptor> Pushes;
    PipelineLayoutDescriptor GetDesc() const noexcept override { return {Groups, Pushes}; }
    Nullable<BindingGroupLayoutValidation*> FindGroup(uint32_t slot) const noexcept {
        const auto group = std::find_if(Groups.begin(), Groups.end(), [&](const auto& g) { return g.GroupSlot == slot; });
        return group == Groups.end() ? Nullable<BindingGroupLayoutValidation*>{nullptr} : static_cast<BindingGroupLayoutValidation*>(group->Layout);
    }
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

class ShaderParameterSetValidation final : public ValidatedObject<ShaderParameterSet> {
public:
    ShaderParameterSetValidation(shared_ptr<ValidationContext> context, unique_ptr<ShaderParameterSet> native, BindingGroupLayoutValidation* layout)
        : ValidatedObject(std::move(context), std::move(native)), Layout(layout), Values(layout->Bindings.size()) {
        AddDependency(layout);
        for (size_t i = 0; i < Values.size(); ++i) Values[i].resize(layout->Bindings[i].Count);
    }
    BindingGroupLayoutValidation* Layout;
    bool Sealed{false};
    vector<vector<std::optional<ShaderParameterValue>>> Values;
    bool ValidateValue(const BindingInfo& binding, const ShaderParameterValue& value, std::string_view operation) const noexcept {
        const auto validateBuffer = [&](Buffer* buffer, BufferRange range, bool texel, uint32_t stride, TextureFormat format) {
            const auto native = Lookup(*Context, buffer, operation);
            if (!native) return false;
            const auto desc = buffer->GetDesc();
            const auto kind = binding.Kind;
            const auto usage = kind == BindingKind::CBuffer ? BufferUse::CBuffer : kind == BindingKind::RWStructuredBuffer || kind == BindingKind::RWRawBuffer || kind == BindingKind::RWTypedBuffer ? BufferUse::UnorderedAccess
                                                                                                                                                                                                     : BufferUse::Resource;
            if (!desc.Usage.HasFlag(usage) || !BufferRangeFits(desc, range)) return Context->Reject(operation, "buffer binding usage or range is invalid");
            const auto size = RangeSize(desc, range);
            const auto& limits = Context->NativeDevice->GetCapabilities().Limits;
            if (texel) {
                if (range.Offset % std::max(uint64_t{1}, limits.TexelBufferOffsetAlignment)) return Context->Reject(operation, "texel buffer offset is not aligned");
            } else if (kind == BindingKind::CBuffer) {
                const auto alignment = std::max(uint64_t{1}, limits.CBufferOffsetAlignment);
                if (range.Offset % alignment || (limits.MaxUniformBufferRange && size > limits.MaxUniformBufferRange)) return Context->Reject(operation, "constant buffer range or alignment is invalid");
            } else if (Context->NativeDevice->GetBackend() == RenderBackend::Vulkan) {
                if (range.Offset % std::max(uint64_t{1}, limits.StorageBufferOffsetAlignment) || (limits.MaxStorageBufferRange && size > limits.MaxStorageBufferRange))
                    return Context->Reject(operation, "storage buffer range or alignment is invalid");
            }
            if (texel && (!ValidEnum(format) || format == TextureFormat::UNKNOWN || IsDepthStencilFormat(format) || range.Offset % GetTextureFormatBytesPerPixel(format) || size % GetTextureFormatBytesPerPixel(format)))
                return Context->Reject(operation, "texel buffer format or range is invalid");
            if ((kind == BindingKind::StructuredBuffer || kind == BindingKind::RWStructuredBuffer) && (!stride || size % stride || range.Offset % stride))
                return Context->Reject(operation, "structured buffer stride or range is invalid");
            if ((kind == BindingKind::RawBuffer || kind == BindingKind::RWRawBuffer) && (stride || range.Offset % 4 || size % 4))
                return Context->Reject(operation, "raw buffers require zero stride and a four-byte aligned range");
#ifdef RADRAY_ENABLE_D3D12
            if (Context->NativeDevice->GetBackend() == RenderBackend::D3D12) {
                if (stride > D3D12_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES)
                    return Context->Reject(operation, "structured buffer stride exceeds the D3D12 limit");
                if (texel) {
                    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
                    support.Format = d3d12::MapType(format);
                    const auto required = D3D12_FORMAT_SUPPORT1_BUFFER | (kind == BindingKind::RWTypedBuffer ? D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW : D3D12_FORMAT_SUPPORT1_SHADER_LOAD);
                    const auto device = static_cast<d3d12::DeviceD3D12*>(Context->NativeDevice.get());
                    if (FAILED(device->_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) || (support.Support1 & required) != required)
                        return Context->Reject(operation, "typed buffer format is unsupported by D3D12");
                }
            }
#endif
            return true;
        };
        switch (binding.Kind) {
            case BindingKind::CBuffer:
            case BindingKind::StructuredBuffer:
            case BindingKind::RWStructuredBuffer:
            case BindingKind::RawBuffer:
            case BindingKind::RWRawBuffer: {
                const auto* input = std::get_if<ShaderBufferBinding>(&value);
                return input ? validateBuffer(input->Target, input->Range, false, input->StructureByteStride, TextureFormat::UNKNOWN) : Context->Reject(operation, "binding requires ShaderBufferBinding");
            }
            case BindingKind::TypedBuffer:
            case BindingKind::RWTypedBuffer: {
                const auto* input = std::get_if<ShaderTexelBufferBinding>(&value);
                return input ? validateBuffer(input->Target, input->Range, true, 0, input->Format) : Context->Reject(operation, "binding requires ShaderTexelBufferBinding");
            }
            case BindingKind::Texture:
            case BindingKind::RWTexture: {
                const auto* input = std::get_if<TextureView*>(&value);
                if (!input) return Context->Reject(operation, "binding requires TextureView");
                const auto native = Lookup(*Context, *input, operation);
                if (!native) return false;
                const auto desc = (*input)->GetDesc();
                const auto target = Lookup(*Context, desc.Target, operation);
                if (!target) return false;
                const auto expected = binding.Kind == BindingKind::RWTexture ? TextureViewUsage::UnorderedAccess : TextureViewUsage::Resource;
                return desc.Usage == expected || Context->Reject(operation, "texture view usage does not match the binding");
            }
            case BindingKind::Sampler: {
                const auto* input = std::get_if<Sampler*>(&value);
                return input ? bool(Lookup(*Context, *input, operation)) : Context->Reject(operation, "binding requires Sampler");
            }
        }
        return Context->Reject(operation, "unsupported binding kind");
    }
    ShaderParameterValue Unwrap(ShaderParameterValue value) const noexcept {
        std::visit([&](auto& input) {
            using V = std::decay_t<decltype(input)>;
            if constexpr (std::is_same_v<V, ShaderBufferBinding> || std::is_same_v<V, ShaderTexelBufferBinding>)
                input.Target = static_cast<Buffer*>(Context->Find(input.Target).Get()->Native);
            else
                input = static_cast<std::remove_pointer_t<V>*>(Context->Find(input).Get()->Native);
        },
                   value);
        return value;
    }
    bool Set(uint32_t bindingId, uint32_t element, ShaderParameterValue value) noexcept override {
        if (!Ready("Set")) return false;
        const auto binding = Layout->Resolve(bindingId, "Set");
        if (!binding) return false;
        if (Sealed || element >= binding.Get()->Count || binding.Get()->Immutable)
            return Context->Reject("Set", "parameter set is sealed, or group, array element, or immutable slot is invalid");
        if (!ValidateValue(*binding.Get(), value, "Set")) return false;
        if (!Real()->Set(bindingId, element, Unwrap(value))) return false;
        Values[static_cast<size_t>(binding.Get() - Layout->Bindings.data())][element] = value;
        return true;
    }
    bool FlushWrites() noexcept override { return Ready("FlushWrites") && Real()->FlushWrites(); }
    void ResetBindingsForReuse() noexcept override {
        if (!Ready("ResetBindingsForReuse")) return;
        bool active;
        {
            std::lock_guard lock{Context->Mutex};
            active = Dependents != 0;
        }
        if (active) {
            Context->Reject("ResetBindingsForReuse", "set is still bound by an active encoder");
            return;
        }
        Real()->ResetBindingsForReuse();
        for (auto& values : Values)
            for (auto& value : values) value.reset();
        Sealed = false;
    }
};

class GraphicsPipelineValidation final : public ValidatedObject<GraphicsPipelineState> {
public:
    GraphicsPipelineValidation(shared_ptr<ValidationContext> context, unique_ptr<GraphicsPipelineState> native, PipelineLayoutValidation* layout, const GraphicsPipelineStateDescriptor& desc)
        : ValidatedObject(std::move(context), std::move(native)), Layout(layout), Signature(GetAttachmentSignature(desc)), Buffers(desc.VertexInput.Buffers.begin(), desc.VertexInput.Buffers.end()), Primitive(desc.Primitive) {
        AddDependency(layout);
        if (desc.DepthStencil) {
            const auto& depth = *desc.DepthStencil;
            const auto writes = [](const StencilFaceState& face) {
                return face.FailOp != StencilOperation::Keep || face.DepthFailOp != StencilOperation::Keep || face.PassOp != StencilOperation::Keep;
            };
            WritesDepth = depth.DepthWriteEnable;
            WritesStencil = depth.Stencil && depth.Stencil->WriteMask && (writes(depth.Stencil->Front) || writes(depth.Stencil->Back));
        }
    }
    PipelineLayoutValidation* Layout;
    AttachmentSignature Signature;
    vector<VertexBufferLayout> Buffers;
    PrimitiveState Primitive;
    bool WritesDepth{false}, WritesStencil{false};
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

class ComputePipelineValidation final : public ValidatedObject<ComputePipelineState> {
public:
    ComputePipelineValidation(shared_ptr<ValidationContext> context, unique_ptr<ComputePipelineState> native, PipelineLayoutValidation* layout)
        : ValidatedObject(std::move(context), std::move(native)), Layout(layout) { AddDependency(layout); }
    PipelineLayoutValidation* Layout;
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

class CommandBufferValidation;

class CommandAllocatorValidation final : public ValidatedObject<CommandAllocator> {
public:
    CommandAllocatorValidation(shared_ptr<ValidationContext> context, unique_ptr<CommandAllocator> native, CommandQueue* queue)
        : ValidatedObject(std::move(context), std::move(native)), Queue(queue) { AddDependency(Context->Find(queue).Get()); }
    CommandQueue* Queue;
    vector<CommandBufferValidation*> Children;
    Nullable<CommandBufferValidation*> RecordingCommand{nullptr};
    void Reset() noexcept override;
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

class CommandBufferValidation final : public ValidatedObject<CommandBuffer> {
public:
    enum class RecordingState { Initial,
                                Recording,
                                Executable,
                                Submitted };
    CommandBufferValidation(shared_ptr<ValidationContext> context, unique_ptr<CommandBuffer> native, CommandAllocatorValidation* allocator)
        : ValidatedObject(std::move(context), std::move(native)), Allocator(allocator) {
        AddDependency(allocator);
        Allocator->Children.push_back(this);
    }
    ~CommandBufferValidation() noexcept override { Destroy(); }
    CommandAllocatorValidation* Allocator;
    RecordingState State{RecordingState::Initial};
    Nullable<ObjectRecord*> Active{nullptr};
    uint32_t DebugDepth{0};
    vector<ObjectRecord*> EpochReferences;
    void Destroy() noexcept override {
        if (!Live) return;
        if (!CanDestroy("Destroy")) {
            Context->Reject("Destroy", "command has an open recording or encoder");
            return;
        }
        Real()->Destroy();
        std::erase(Allocator->Children, this);
        Retire();
    }
    void ResetLogical() noexcept {
        for (auto* reference : EpochReferences) RemoveDependency(reference);
        EpochReferences.clear();
        State = RecordingState::Initial;
        DebugDepth = 0;
    }
    void Pin(ObjectRecord* reference) {
        if (std::find(EpochReferences.begin(), EpochReferences.end(), reference) != EpochReferences.end()) return;
        EpochReferences.push_back(reference);
        AddDependency(reference);
    }
    bool Recording(std::string_view operation, bool outside = true) const noexcept {
        if (!Ready(operation)) return false;
        return (State == RecordingState::Recording && (!outside || !Active)) || Context->Reject(operation, "command requires recording in the appropriate encoder scope");
    }
    void Begin() noexcept override {
        if (!Ready("Begin")) return;
        if (State != RecordingState::Initial) {
            Context->Reject("Begin", "command must be new or reset by its allocator");
            return;
        }
        const bool d3d12 = Context->NativeDevice->GetBackend() == RenderBackend::D3D12;
        if (d3d12 && Allocator->RecordingCommand) {
            Context->Reject("Begin", "D3D12 allocator already has an open recording");
            return;
        }
        Real()->Begin();
        if (d3d12) Allocator->RecordingCommand = this;
        State = RecordingState::Recording;
        Open = true;
    }
    void End() noexcept override {
        if (!Recording("End")) return;
        if (DebugDepth) {
            Context->Reject("End", "debug groups are unbalanced");
            return;
        }
        Real()->End();
        if (Context->NativeDevice->GetBackend() == RenderBackend::D3D12) Allocator->RecordingCommand = nullptr;
        State = RecordingState::Executable;
        Open = false;
    }
    void PushDebugGroup(std::string_view name) noexcept override {
        if (Recording("PushDebugGroup", false)) {
            Real()->PushDebugGroup(name);
            ++DebugDepth;
        }
    }
    void PopDebugGroup() noexcept override {
        if (!Recording("PopDebugGroup", false)) return;
        if (!DebugDepth) {
            Context->Reject("PopDebugGroup", "no debug group is open");
            return;
        }
        Real()->PopDebugGroup();
        --DebugDepth;
    }
    void ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept override;
    Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassBeginDescriptor& desc) noexcept override;
    void EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept override;
    Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept override;
    void EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept override;
    void CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept override;
    void CopyBufferToTexture(Texture* dst, SubresourceRange range, Buffer* src, uint64_t offset) noexcept override;
    bool CopyBufferToTextureRegion(const BufferToTextureCopyDescriptor& desc) noexcept override;
    void CopyTextureToBuffer(Buffer* dst, uint64_t offset, Texture* src, SubresourceRange range) noexcept override;
    void CopyTextureToTexture(const TextureCopyDescriptor& desc) noexcept override;
    void ResolveTexture(const TextureResolveDescriptor& desc) noexcept override;
    void ResetQueryPool(QueryPool* pool, uint32_t first, uint32_t count) noexcept override;
    void WriteTimestamp(const QueryTimestampDescriptor& desc) noexcept override;
    void ResolveQueryData(const QueryResolveDescriptor& desc) noexcept override;
    bool ValidateTextureBufferCopy(Texture* texture, SubresourceRange range, Buffer* buffer, uint64_t offset, bool toTexture, std::string_view operation) const noexcept;
    bool ValidateQueryQueue(std::string_view operation, std::optional<QueryPipelineStage> stage = std::nullopt) const noexcept;
    void SetDebugName(std::string_view name) noexcept override { ValidatedObject::ForwardDebugName(name); }
};

void CommandAllocatorValidation::Reset() noexcept {
    if (!Ready("Reset")) return;
    for (auto* child : Children)
        if (child->State == CommandBufferValidation::RecordingState::Recording) {
            Context->Reject("Reset", "allocator has an open recording");
            return;
        }
    Real()->Reset();
    for (auto* child : Children) child->ResetLogical();
}

template <class T>
class EncoderValidation : public ValidatedObject<T> {
public:
    EncoderValidation(shared_ptr<ValidationContext> context, unique_ptr<T> native, CommandBufferValidation* command)
        : ValidatedObject<T>(std::move(context), std::move(native)), Command(command) {
        this->Open = true;
        this->AddDependency(command);
    }
    CommandBufferValidation* Command;
    Nullable<PipelineLayoutValidation*> Layout{nullptr};
    unordered_map<uint32_t, ShaderParameterSetValidation*> BoundSets;
    bool Active(std::string_view operation) const noexcept {
        if (!this->Ready(operation)) return false;
        return (this->Open && Command->State == CommandBufferValidation::RecordingState::Recording && Command->Active.Get() == this) || this->Context->Reject(operation, "encoder is not active on its command buffer");
    }
    CommandBuffer* GetCommandBuffer() const noexcept override {
        if (!this->Ready("GetCommandBuffer")) this->Context->Fatal("GetCommandBuffer", "cannot query a destroyed encoder");
        return Command;
    }
    void BindShaderParameterSet(uint32_t group, ShaderParameterSet* input, std::span<const ShaderParameterDynamicOffset> offsets) noexcept override {
        if (!Active("BindShaderParameterSet")) return;
        if (!Layout) {
            this->Context->Reject("BindShaderParameterSet", "bind a pipeline before its parameters");
            return;
        }
        const auto set = WrapperOf<ShaderParameterSetValidation>(*this->Context, input, "BindShaderParameterSet");
        if (!set) return;
        const auto target = Layout.Get()->FindGroup(group);
        if (!target) {
            this->Context->Reject("BindShaderParameterSet", "unknown group slot");
            return;
        }
        for (const auto& expected : target.Get()->Bindings) {
            if (expected.Immutable) continue;
            const auto actual = set.Get()->Layout->Resolve(expected.Binding, "BindShaderParameterSet");
            if (!actual || actual.Get()->Kind != expected.Kind || actual.Get()->Count < expected.Count) {
                this->Context->Reject("BindShaderParameterSet", "group slot cannot accept the supplied resource type or count");
                return;
            }
            const auto index = static_cast<size_t>(actual.Get() - set.Get()->Layout->Bindings.data());
            for (uint32_t element = 0; element < expected.Count; ++element) {
                const auto& value = set.Get()->Values[index][element];
                if (!value || !set.Get()->ValidateValue(expected, *value, "BindShaderParameterSet")) {
                    this->Context->Reject("BindShaderParameterSet", "required resource is unset or invalid");
                    return;
                }
            }
        }
        unordered_set<uint32_t> seen;
        size_t expectedCount = 0;
        for (const auto& binding : target.Get()->Bindings)
            if (binding.Dynamic) ++expectedCount;
        for (const auto& offset : offsets) {
            const auto binding = target.Get()->Resolve(offset.BindingId, "BindShaderParameterSet");
            if (!binding || !binding.Get()->Dynamic || offset.ArrayIndex != 0 || !seen.insert(offset.BindingId).second) {
                this->Context->Reject("BindShaderParameterSet", "unexpected or duplicate dynamic offset");
                return;
            }
            const auto source = set.Get()->Layout->Resolve(offset.BindingId, "BindShaderParameterSet");
            if (!source) return;
            const auto index = static_cast<size_t>(source.Get() - set.Get()->Layout->Bindings.data());
            const auto& value = set.Get()->Values[index][0];
            const auto* buffer = value ? std::get_if<ShaderBufferBinding>(&*value) : nullptr;
            if (!buffer) {
                this->Context->Reject("BindShaderParameterSet", "dynamic buffer is unset");
                return;
            }
            const auto desc = buffer->Target->GetDesc();
            const auto& limits = this->Context->NativeDevice->GetCapabilities().Limits;
            const auto alignment = binding.Get()->Kind == BindingKind::CBuffer ? limits.CBufferOffsetAlignment : limits.StorageBufferOffsetAlignment;
            if (offset.Offset % std::max(uint64_t{1}, alignment) || offset.Offset > desc.Size - buffer->Range.Offset ||
                RangeSize(desc, buffer->Range) > desc.Size - buffer->Range.Offset - offset.Offset) {
                this->Context->Reject("BindShaderParameterSet", "dynamic buffer offset exceeds range or alignment");
                return;
            }
        }
        if (seen.size() != expectedCount) {
            this->Context->Reject("BindShaderParameterSet", "each dynamic binding requires exactly one offset");
            return;
        }
        this->Real()->BindShaderParameterSet(group, set.Get()->Real(), offsets);
        this->AddDependency(set.Get());
        set.Get()->Sealed = true;
        BoundSets[group] = set.Get();
    }
    bool SetPushConstants(uint32_t pushId, uint32_t byteOffset, std::span<const byte> data) noexcept override {
        if (!Active("SetPushConstants")) return false;
        if (!Layout) return this->Context->Reject("SetPushConstants", "bind a pipeline before push constants");
        const auto& pushes = Layout.Get()->Pushes;
        const auto push = std::find_if(pushes.begin(), pushes.end(), [&](const auto& p) { return p.PushId == pushId; });
        if (push == pushes.end() || byteOffset % 4 || data.empty() || data.size() % 4 || byteOffset > push->Size || data.size() > push->Size - byteOffset)
            return this->Context->Reject("SetPushConstants", "push ID, alignment or range is invalid");
        return this->Real()->SetPushConstants(pushId, byteOffset, data);
    }
    bool ParametersReady(std::string_view operation) const noexcept {
        for (const auto& group : Layout.Get()->Groups) {
            auto* layout = static_cast<BindingGroupLayoutValidation*>(group.Layout);
            if (layout->Bindings.empty() ||
                (this->Context->NativeDevice->GetBackend() == RenderBackend::D3D12 &&
                 std::none_of(layout->Bindings.begin(), layout->Bindings.end(), [](const auto& b) { return !b.Immutable; }))) continue;
            const auto found = BoundSets.find(group.GroupSlot);
            if (found == BoundSets.end() || !this->Context->Find(found->second) || !found->second->Ready(operation))
                return this->Context->Reject(operation, "parameter group is not bound or was destroyed");
            const auto& set = *found->second;
            for (size_t i = 0; i < set.Values.size(); ++i) {
                const auto& binding = set.Layout->Bindings[i];
                if (binding.Immutable) continue;
                for (const auto& value : set.Values[i])
                    if (!value || !set.ValidateValue(binding, *value, operation)) return false;
            }
        }
        return true;
    }
    bool Indirect(Buffer* buffer, uint64_t offset, uint64_t bytes, std::string_view operation) const noexcept {
        if (!Lookup(*this->Context, buffer, operation)) return false;
        const auto desc = buffer->GetDesc();
        return (desc.Usage.HasFlag(BufferUse::Indirect) && !(offset % 4) && RangeFits(desc.Size, offset, bytes, true)) || this->Context->Reject(operation, "indirect argument usage, alignment, or range is invalid");
    }
};

class GraphicsEncoderValidation final : public EncoderValidation<GraphicsCommandEncoder> {
public:
    GraphicsEncoderValidation(shared_ptr<ValidationContext> context, unique_ptr<GraphicsCommandEncoder> native, CommandBufferValidation* command, AttachmentSignature signature, TextureViewUsage depthUsage)
        : EncoderValidation(std::move(context), std::move(native), command), Signature(std::move(signature)), DepthUsage(depthUsage) {}
    AttachmentSignature Signature;
    TextureViewUsage DepthUsage{TextureViewUsage::UNKNOWN};
    Nullable<GraphicsPipelineValidation*> Pipeline{nullptr};
    unordered_map<uint32_t, VertexBufferView> VertexBuffers;
    std::optional<IndexBufferView> IndexBuffer;
    void SetViewport(Viewport viewport) noexcept override {
        if (!Active("SetViewport")) return;
        if (!std::isfinite(viewport.X) || !std::isfinite(viewport.Y) || !std::isfinite(viewport.Width) || !std::isfinite(viewport.Height) || !std::isfinite(viewport.MinDepth) || !std::isfinite(viewport.MaxDepth) || viewport.Width <= 0 || viewport.Height == 0 || (viewport.Height < 0 && Context->NativeDevice->GetBackend() != RenderBackend::Vulkan) || viewport.MinDepth < 0 || viewport.MaxDepth > 1 || viewport.MinDepth > viewport.MaxDepth) {
            Context->Reject("SetViewport", "viewport bounds or depth range is invalid");
            return;
        }
        Real()->SetViewport(viewport);
    }
    void SetScissor(Rect rect) noexcept override {
        if (!Active("SetScissor")) return;
        if (rect.X < 0 || rect.Y < 0 || !rect.Width || !rect.Height || uint64_t(rect.X) + rect.Width > INT32_MAX || uint64_t(rect.Y) + rect.Height > INT32_MAX) {
            Context->Reject("SetScissor", "scissor bounds are invalid");
            return;
        }
        Real()->SetScissor(rect);
    }
    void BindGraphicsPipelineState(GraphicsPipelineState* input) noexcept override {
        if (!Active("BindGraphicsPipelineState")) return;
        const auto pipeline = WrapperOf<GraphicsPipelineValidation>(*Context, input, "BindGraphicsPipelineState");
        if (!pipeline) return;
        if (pipeline.Get()->Signature != Signature) {
            Context->Reject("BindGraphicsPipelineState", "pipeline attachment signature does not match render scope");
            return;
        }
        if ((IsDepthReadOnly(DepthUsage) && pipeline.Get()->WritesDepth) || (IsStencilReadOnly(DepthUsage) && pipeline.Get()->WritesStencil)) {
            Context->Reject("BindGraphicsPipelineState", "pipeline writes a read-only depth/stencil attachment");
            return;
        }
        Real()->BindGraphicsPipelineState(pipeline.Get()->Real());
        AddDependency(pipeline.Get());
        if (Layout.Get() != pipeline.Get()->Layout) BoundSets.clear();
        if (Pipeline.Get() != pipeline.Get()) VertexBuffers.clear();
        Pipeline = pipeline;
        Layout = pipeline.Get()->Layout;
    }
    void BindVertexBuffers(std::span<const VertexBufferBinding> bindings) noexcept override {
        if (!Active("BindVertexBuffers")) return;
        if (!Pipeline) {
            Context->Reject("BindVertexBuffers", "bind a pipeline before vertex buffers");
            return;
        }
        vector<VertexBufferBinding> nativeBindings;
        unordered_set<uint32_t> seen;
        uint32_t low = UINT32_MAX, high = 0;
        const auto maximum = Context->NativeDevice->GetDetail().MaxVertexInputBindings;
        for (const auto& binding : bindings) {
            const auto native = Lookup(*Context, binding.View.Target, "BindVertexBuffers");
            if (!native) return;
            const auto desc = binding.View.Target->GetDesc();
            if (binding.Binding >= maximum || !seen.insert(binding.Binding).second || !desc.Usage.HasFlag(BufferUse::Vertex) || !RangeFits(desc.Size, binding.View.Offset, binding.View.Size) || binding.View.Size > UINT32_MAX) {
                Context->Reject("BindVertexBuffers", "vertex buffer slot, usage, or range is invalid");
                return;
            }
            nativeBindings.push_back({binding.Binding, {native.Get(), binding.View.Offset, binding.View.Size}});
            low = std::min(low, binding.Binding);
            high = std::max(high, binding.Binding);
        }
        if (!bindings.empty() && high - low + 1 != bindings.size()) {
            Context->Reject("BindVertexBuffers", "vertex buffer bindings must be contiguous");
            return;
        }
        Real()->BindVertexBuffers(nativeBindings);
        for (const auto& binding : bindings) VertexBuffers[binding.Binding] = binding.View;
    }
    void BindIndexBuffer(IndexBufferView view) noexcept override {
        if (!Active("BindIndexBuffer")) return;
        const auto native = Lookup(*Context, view.Target, "BindIndexBuffer");
        if (!native) return;
        const auto desc = view.Target->GetDesc();
        if (!desc.Usage.HasFlag(BufferUse::Index) || (view.Stride != 2 && view.Stride != 4) || view.Offset >= desc.Size || view.Offset % view.Stride) {
            Context->Reject("BindIndexBuffer", "index buffer usage, stride, or offset is invalid");
            return;
        }
        auto converted = view;
        converted.Target = native.Get();
        Real()->BindIndexBuffer(converted);
        IndexBuffer = view;
    }
    bool DrawReady(std::string_view operation, bool indexed) const noexcept {
        if (!Active(operation)) return false;
        if (!Pipeline || !Context->Find(Pipeline.Get()) || !Pipeline.Get()->Ready(operation)) return Context->Reject(operation, "graphics pipeline is not bound or has been destroyed");
        if (!ParametersReady(operation)) return false;
        for (const auto& binding : Pipeline.Get()->Buffers) {
            const auto found = VertexBuffers.find(binding.Binding);
            if (found == VertexBuffers.end() || !Lookup(*Context, found->second.Target, operation)) return Context->Reject(operation, "required vertex buffer has not been bound");
        }
        if (indexed) {
            if (!IndexBuffer || !Lookup(*Context, IndexBuffer->Target, operation)) return Context->Reject(operation, "index buffer has not been bound");
            if (Pipeline.Get()->Primitive.StripIndexFormat && GetIndexFormatSizeInBytes(*Pipeline.Get()->Primitive.StripIndexFormat) != IndexBuffer->Stride) return Context->Reject(operation, "index format does not match primitive restart format");
        }
        return true;
    }
    void Draw(uint32_t count, uint32_t instances, uint32_t first, uint32_t firstInstance) noexcept override {
        if (!DrawReady("Draw", false)) return;
        if (count && instances)
            for (const auto& binding : Pipeline.Get()->Buffers) {
                const auto& view = VertexBuffers.at(binding.Binding);
                const uint64_t end = binding.StepMode == VertexStepMode::Vertex ? uint64_t(first) + count : uint64_t(firstInstance) + instances;
                if (end * binding.ArrayStride > view.Size) {
                    Context->Reject("Draw", "vertex or instance buffer range is too small");
                    return;
                }
            }
        Real()->Draw(count, instances, first, firstInstance);
    }
    void DrawIndexed(uint32_t count, uint32_t instances, uint32_t first, int32_t vertexOffset, uint32_t firstInstance) noexcept override {
        if (!DrawReady("DrawIndexed", true)) return;
        if (instances && (uint64_t(first) + count) * IndexBuffer->Stride > IndexBuffer->Target->GetDesc().Size - IndexBuffer->Offset) {
            Context->Reject("DrawIndexed", "index buffer range is too small");
            return;
        }
        Real()->DrawIndexed(count, instances, first, vertexOffset, firstInstance);
    }
    void DrawIndirect(Buffer* buffer, uint64_t offset, uint32_t count) noexcept override {
        if (DrawReady("DrawIndirect", false) && (Context->NativeDevice->GetCapabilities().Features.IndirectDraw || Context->Reject("DrawIndirect", "indirect draws are unsupported")) && Indirect(buffer, offset, uint64_t(count) * sizeof(DrawIndirectArguments), "DrawIndirect")) Real()->DrawIndirect(Lookup(*Context, buffer, "DrawIndirect").Get(), offset, count);
    }
    void DrawIndexedIndirect(Buffer* buffer, uint64_t offset, uint32_t count) noexcept override {
        if (DrawReady("DrawIndexedIndirect", true) && (Context->NativeDevice->GetCapabilities().Features.IndirectDraw || Context->Reject("DrawIndirect", "indirect draws are unsupported")) && Indirect(buffer, offset, uint64_t(count) * sizeof(DrawIndexedIndirectArguments), "DrawIndexedIndirect")) Real()->DrawIndexedIndirect(Lookup(*Context, buffer, "DrawIndexedIndirect").Get(), offset, count);
    }
};

class ComputeEncoderValidation final : public EncoderValidation<ComputeCommandEncoder> {
public:
    using EncoderValidation::EncoderValidation;
    Nullable<ComputePipelineValidation*> Pipeline{nullptr};
    void BindComputePipelineState(ComputePipelineState* input) noexcept override {
        if (!Active("BindComputePipelineState")) return;
        const auto pipeline = WrapperOf<ComputePipelineValidation>(*Context, input, "BindComputePipelineState");
        if (!pipeline) return;
        Real()->BindComputePipelineState(pipeline.Get()->Real());
        AddDependency(pipeline.Get());
        if (Layout.Get() != pipeline.Get()->Layout) BoundSets.clear();
        Layout = pipeline.Get()->Layout;
        Pipeline = pipeline;
    }
    bool DispatchReady(std::string_view operation) const noexcept {
        if (!Active(operation)) return false;
        return (Pipeline && Context->Find(Pipeline.Get()) && Pipeline.Get()->Ready(operation) && ParametersReady(operation)) || Context->Reject(operation, "compute pipeline or parameters are not bound");
    }
    void Dispatch(uint32_t x, uint32_t y, uint32_t z) noexcept override {
        if (!DispatchReady("Dispatch")) return;
        uint32_t limits[]{65535, 65535, 65535};
#ifdef RADRAY_ENABLE_VULKAN
        if (auto* device = dynamic_cast<vulkan::DeviceVulkan*>(Context->NativeDevice.get())) std::copy_n(device->_properties.limits.maxComputeWorkGroupCount, 3, limits);
#endif
        if (x > limits[0] || y > limits[1] || z > limits[2]) {
            Context->Reject("Dispatch", "group count exceeds device limits");
            return;
        }
        Real()->Dispatch(x, y, z);
    }
    void DispatchIndirect(Buffer* buffer, uint64_t offset) noexcept override {
        if (DispatchReady("DispatchIndirect") && (Context->NativeDevice->GetCapabilities().Features.IndirectDispatch || Context->Reject("DispatchIndirect", "indirect dispatch is unsupported")) && Indirect(buffer, offset, sizeof(DispatchIndirectArguments), "DispatchIndirect")) Real()->DispatchIndirect(Lookup(*Context, buffer, "DispatchIndirect").Get(), offset);
    }
};

Nullable<unique_ptr<GraphicsCommandEncoder>> CommandBufferValidation::BeginRenderPass(const RenderPassBeginDescriptor& desc) noexcept {
    if (!Recording("BeginRenderPass")) return nullptr;
    if (Allocator->Queue->GetQueueType() != QueueType::Direct) {
        Context->Reject("BeginRenderPass", "render pass requires a graphics queue");
        return nullptr;
    }
    vector<RenderPassColorAttachment> colors(desc.Colors.begin(), desc.Colors.end());
    auto converted = desc;
    for (auto& color : colors) {
        const auto view = Lookup(*Context, color.View, "BeginRenderPass");
        if (!view) return nullptr;
        color.View = view.Get();
        if (color.ResolveTarget) {
            const auto target = Lookup(*Context, color.ResolveTarget.Get(), "BeginRenderPass");
            if (!target) return nullptr;
            color.ResolveTarget = target.Get();
        }
    }
    if (converted.DepthStencil) {
        const auto view = Lookup(*Context, converted.DepthStencil->View, "BeginRenderPass");
        if (!view) return nullptr;
        converted.DepthStencil->View = view.Get();
        if (converted.DepthStencil->Resolve) {
            const auto target = Lookup(*Context, converted.DepthStencil->Resolve->Target, "BeginRenderPass");
            if (!target) return nullptr;
            converted.DepthStencil->Resolve->Target = target.Get();
        }
    }
    const auto signature = ValidateRenderScope(desc, *Context->Owner.Get());
    if (!signature) {
        Context->Reject("BeginRenderPass", "attachments, load/store operations, or render area are invalid");
        return nullptr;
    }
    converted.Colors = colors;
    auto native = Real()->BeginRenderPass(converted);
    if (!native) return nullptr;
    auto encoder = make_unique<GraphicsEncoderValidation>(Context, native.Release(), this, *signature, desc.DepthStencil ? desc.DepthStencil->View->GetDesc().Usage : TextureViewUsage::UNKNOWN);
    for (const auto& color : desc.Colors) {
        Pin(Context->Find(color.View).Get());
        if (color.ResolveTarget) Pin(Context->Find(color.ResolveTarget.Get()).Get());
    }
    if (desc.DepthStencil) {
        Pin(Context->Find(desc.DepthStencil->View).Get());
        if (desc.DepthStencil->Resolve) Pin(Context->Find(desc.DepthStencil->Resolve->Target).Get());
    }
    Active = encoder.get();
    return encoder;
}

void CommandBufferValidation::EndRenderPass(unique_ptr<GraphicsCommandEncoder> encoder) noexcept {
    if (!Recording("EndRenderPass", false)) RADRAY_ABORT("RHI validation: EndRenderPass: command is not recording");
    const auto wrapper = WrapperOf<GraphicsEncoderValidation>(*Context, encoder.get(), "EndRenderPass");
    if (!wrapper) RADRAY_ABORT("RHI validation: EndRenderPass: invalid encoder");
    if (Active.Get() != wrapper.Get()) {
        Context->Fatal("EndRenderPass", "encoder belongs to another command or pass");
        return;
    }
    auto native = wrapper.Get()->TakeNative();
    Real()->EndRenderPass(std::move(native));
    wrapper.Get()->Open = false;
    wrapper.Get()->Retire();
    Active = nullptr;
    encoder.reset();
}

Nullable<unique_ptr<ComputeCommandEncoder>> CommandBufferValidation::BeginComputePass() noexcept {
    if (!Recording("BeginComputePass")) return nullptr;
    if (Allocator->Queue->GetQueueType() == QueueType::Copy) {
        Context->Reject("BeginComputePass", "compute pass is not supported on a copy queue");
        return nullptr;
    }
    auto native = Real()->BeginComputePass();
    if (!native) return nullptr;
    auto encoder = make_unique<ComputeEncoderValidation>(Context, native.Release(), this);
    Active = encoder.get();
    return encoder;
}

void CommandBufferValidation::EndComputePass(unique_ptr<ComputeCommandEncoder> encoder) noexcept {
    if (!Recording("EndComputePass", false)) RADRAY_ABORT("RHI validation: EndComputePass: command is not recording");
    const auto wrapper = WrapperOf<ComputeEncoderValidation>(*Context, encoder.get(), "EndComputePass");
    if (!wrapper) RADRAY_ABORT("RHI validation: EndComputePass: invalid encoder");
    if (Active.Get() != wrapper.Get()) {
        Context->Fatal("EndComputePass", "encoder belongs to another command or pass");
        return;
    }
    auto native = wrapper.Get()->TakeNative();
    Real()->EndComputePass(std::move(native));
    wrapper.Get()->Open = false;
    wrapper.Get()->Retire();
    Active = nullptr;
    encoder.reset();
}

bool BufferStateUsage(BufferStates states, BufferUses usage) noexcept {
    if (!states || (states.value() & ~((uint32_t(BufferState::HostWrite) << 1) - 1))) return false;
    const std::pair<BufferState, BufferUse> requirements[]{
        {BufferState::CopySource, BufferUse::CopySource}, {BufferState::CopyDestination, BufferUse::CopyDestination}, {BufferState::Vertex, BufferUse::Vertex}, {BufferState::Index, BufferUse::Index}, {BufferState::CBuffer, BufferUse::CBuffer}, {BufferState::ShaderRead, BufferUse::Resource}, {BufferState::UnorderedAccess, BufferUse::UnorderedAccess}, {BufferState::Indirect, BufferUse::Indirect}, {BufferState::HostRead, BufferUse::MapRead}, {BufferState::HostWrite, BufferUse::MapWrite}};
    for (const auto& [state, use] : requirements)
        if (states.HasFlag(state) && !usage.HasFlag(use)) return false;
    return true;
}

bool TextureStateUsage(TextureStates states, TextureUses usage) noexcept {
    if ((states.HasFlag(TextureState::DepthReadStencilWrite) || states.HasFlag(TextureState::DepthWriteStencilRead)) &&
        (!usage.HasFlag(TextureUse::DepthStencilRead) || !usage.HasFlag(TextureUse::DepthStencilWrite))) return false;
    if (states.HasFlag(TextureState::AttachmentResolveDestination) && (!usage.HasFlag(TextureUse::RenderTarget) && !usage.HasFlag(TextureUse::DepthStencilWrite))) return false;
    if (!states || (states.value() & ~((uint32_t(TextureState::AttachmentResolveDestination) << 1) - 1))) return false;
    const std::pair<TextureState, TextureUse> requirements[]{
        {TextureState::CopySource, TextureUse::CopySource}, {TextureState::CopyDestination, TextureUse::CopyDestination}, {TextureState::ShaderRead, TextureUse::Resource}, {TextureState::UnorderedAccess, TextureUse::UnorderedAccess}, {TextureState::RenderTarget, TextureUse::RenderTarget}, {TextureState::DepthRead, TextureUse::DepthStencilRead}, {TextureState::DepthWrite, TextureUse::DepthStencilWrite}, {TextureState::ResolveSource, TextureUse::CopySource}, {TextureState::ResolveDestination, TextureUse::CopyDestination}};
    for (const auto& [state, use] : requirements)
        if (states.HasFlag(state) && !usage.HasFlag(use)) return false;
    return true;
}

void CommandBufferValidation::ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept {
    if (!Recording("ResourceBarrier")) return;
    vector<ResourceBarrierDescriptor> converted;
    converted.reserve(barriers.size());
    for (auto barrier : barriers) {
        const bool valid = std::visit([&](auto& value) {
            using V = std::decay_t<decltype(value)>;
            const auto native = Lookup(*Context, value.Target, "ResourceBarrier");
            if (!native) return false;
            if constexpr (std::is_same_v<V, BarrierBufferDescriptor>) {
                const auto desc = value.Target->GetDesc();
                if (!BufferRangeFits(desc, value.Range) || !BufferStateUsage(value.Before, desc.Usage) || !BufferStateUsage(value.After, desc.Usage) || value.After.HasFlag(BufferState::Undefined)) return false;
                if ((value.BeforeStages.value() | value.AfterStages.value()) & ~ShaderStages{ShaderStage::Graphics | ShaderStage::Compute}.value()) return false;
            } else if constexpr (std::is_same_v<V, BarrierTextureDescriptor>) {
                const auto desc = value.Target->GetDesc();
                const auto range = NormalizeSubresourceRange(desc, value.Range);
                if (!range) return false;
                const auto allAspects = GetTextureFormatAspects(desc.Format);
                if (allAspects.HasFlag(TextureAspect::Stencil) && range->Aspects != allAspects && !Context->NativeDevice->GetCapabilities().Features.IndependentDepthStencilTransitions) return false;
                for (const auto state : {value.Before, value.After}) {
                    if (state.HasFlag(TextureState::AttachmentResolveDestination) && state != TextureState::AttachmentResolveDestination) return false;
                    const auto mixed = state.value() & (uint32_t(TextureState::DepthReadStencilWrite) | uint32_t(TextureState::DepthWriteStencilRead));
                    if (mixed && (!allAspects.HasFlag(TextureAspect::Stencil) || range->Aspects != allAspects ||
                                  (mixed & (mixed - 1)) || (state.value() & ~(mixed | uint32_t(TextureState::ShaderRead))))) return false;
                }
                if (!TextureStateUsage(value.Before, desc.Usage) || !TextureStateUsage(value.After, desc.Usage) || value.After.HasFlag(TextureState::Undefined)) return false;
                if ((value.BeforeStages.value() | value.AfterStages.value()) & ~ShaderStages{ShaderStage::Graphics | ShaderStage::Compute}.value()) return false;
            } else {
                if (!Context->NativeDevice->GetCapabilities().Features.UavMemoryBarrier) return false;
                const auto record = Context->Find(value.Target);
                if (const auto buffer = dynamic_cast<BufferValidation*>(record.Get()); buffer && !buffer->Desc.Usage.HasFlag(BufferUse::UnorderedAccess)) return false;
                if (const auto texture = dynamic_cast<TextureValidation*>(record.Get()); texture && !texture->Desc.Usage.HasFlag(TextureUse::UnorderedAccess)) return false;
                if (!dynamic_cast<BufferValidation*>(record.Get()) && !dynamic_cast<TextureValidation*>(record.Get())) return false;
            }
            value.Target = native.Get();
            return true;
        },
                                      barrier);
        if (!valid) {
            Context->Reject("ResourceBarrier", "barrier state, usage, stages, or range is invalid");
            return;
        }
#ifdef RADRAY_ENABLE_VULKAN
        if (Context->NativeDevice->GetBackend() == RenderBackend::Vulkan) {
            const auto flags = static_cast<vulkan::CommandBufferVulkan*>(Real())->_cmdPool->_queue->_queueFlags;
            const bool supported = std::visit([&](const auto& value) {
                using V = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<V, BarrierUavDescriptor>) {
                    return (flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;
                } else {
                    VkPipelineStageFlags before, after;
                    if constexpr (std::is_same_v<V, BarrierBufferDescriptor>) {
                        before = vulkan::BufferStateToPipelineStageFlags(value.Before);
                        after = vulkan::BufferStateToPipelineStageFlags(value.After);
                    } else {
                        before = vulkan::TextureStateToPipelineStageFlags(value.Before, true);
                        after = vulkan::TextureStateToPipelineStageFlags(value.After, false);
                    }
                    return vulkan::ArePipelineStagesSupportedVulkan(vulkan::ResolveShaderPipelineStagesVulkan(before, value.BeforeStages, flags), flags) &&
                           vulkan::ArePipelineStagesSupportedVulkan(vulkan::ResolveShaderPipelineStagesVulkan(after, value.AfterStages, flags), flags);
                }
            },
                                              barrier);
            if (!supported) {
                Context->Reject("ResourceBarrier", "barrier stages are unsupported by this Vulkan queue family");
                return;
            }
        }
#endif
#ifdef RADRAY_ENABLE_D3D12
        if (Context->NativeDevice->GetBackend() == RenderBackend::D3D12) {
            const auto type = static_cast<d3d12::CmdListD3D12*>(Real())->_type;
            const bool supported = std::visit([&](const auto& value) {
                using V = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<V, BarrierUavDescriptor>) {
                    return type != D3D12_COMMAND_LIST_TYPE_COPY;
                } else {
                    const auto stages = value.BeforeStages | value.AfterStages;
                    if (type == D3D12_COMMAND_LIST_TYPE_COPY && stages) return false;
                    if (type == D3D12_COMMAND_LIST_TYPE_COMPUTE && (stages & ShaderStage::Graphics)) return false;
                    const auto validState = [&](D3D12_RESOURCE_STATES state, ShaderStages shaders) {
                        return d3d12::AreResourceStatesSupportedD3D12(d3d12::ResolveShaderResourceStatesD3D12(state, shaders, type), type);
                    };
                    if constexpr (std::is_same_v<V, BarrierBufferDescriptor>) {
                        return validState(d3d12::MapType(value.Before), value.BeforeStages) && validState(d3d12::MapType(value.After), value.AfterStages);
                    } else {
                        const auto range = ResolveSubresourceRange(value.Target->GetDesc(), value.Range);
                        for (const auto aspect : {TextureAspect::Color, TextureAspect::Depth, TextureAspect::Stencil}) {
                            if (range.Aspects.HasFlag(aspect) &&
                                (!validState(d3d12::MapType(TextureStateForAspect(value.Before, aspect)), value.BeforeStages) ||
                                 !validState(d3d12::MapType(TextureStateForAspect(value.After, aspect)), value.AfterStages))) return false;
                        }
                        return true;
                    }
                }
            },
                                              barrier);
            if (!supported) {
                Context->Reject("ResourceBarrier", "resource states or shader stages are unsupported by this D3D12 command list");
                return;
            }
        }
#endif
        converted.push_back(barrier);
    }
    Real()->ResourceBarrier(converted);
}

void CommandBufferValidation::CopyBufferToBuffer(Buffer* dst, uint64_t dstOffset, Buffer* src, uint64_t srcOffset, uint64_t size) noexcept {
    if (!Recording("CopyBufferToBuffer")) return;
    const auto destination = Lookup(*Context, dst, "CopyBufferToBuffer"), source = Lookup(*Context, src, "CopyBufferToBuffer");
    if (!destination || !source) return;
    const auto d = dst->GetDesc(), s = src->GetDesc();
    const auto alignment = std::max<uint64_t>(1, Context->NativeDevice->GetDetail().BufferCopyOffsetAlignment);
    if (!d.Usage.HasFlag(BufferUse::CopyDestination) || !s.Usage.HasFlag(BufferUse::CopySource) || !RangeFits(d.Size, dstOffset, size) || !RangeFits(s.Size, srcOffset, size) || dstOffset % alignment || srcOffset % alignment || (dst == src && dstOffset < srcOffset + size && srcOffset < dstOffset + size)) {
        Context->Reject("CopyBufferToBuffer", "copy usage, alignment, overlap, or range is invalid");
        return;
    }
    Real()->CopyBufferToBuffer(destination.Get(), dstOffset, source.Get(), srcOffset, size);
}

bool CommandBufferValidation::CopyBufferToTextureRegion(const BufferToTextureCopyDescriptor& desc) noexcept {
    if (!Recording("CopyBufferToTextureRegion")) return false;
    const auto source = Lookup(*Context, desc.Source, "CopyBufferToTextureRegion");
    const auto destination = Lookup(*Context, desc.Destination, "CopyBufferToTextureRegion");
    if (!source || !destination) return false;
    const auto [valid, reason] = ValidateBufferTextureCopyRegion(desc.Source->GetDesc(), desc.Destination->GetDesc(), desc.Region, Context->NativeDevice->GetDetail());
    if (!valid) return Context->Reject("CopyBufferToTextureRegion", reason);
    return Real()->CopyBufferToTextureRegion({source.Get(), destination.Get(), desc.Region});
}

bool CommandBufferValidation::ValidateTextureBufferCopy(Texture* texture, SubresourceRange range, Buffer* buffer, uint64_t offset, bool toTexture, std::string_view operation) const noexcept {
    if (!Lookup(*Context, texture, operation) || !Lookup(*Context, buffer, operation)) return false;
    const auto t = texture->GetDesc();
    const auto b = buffer->GetDesc();
    if (range.ArrayLayerCount == SubresourceRange::All || range.MipLevelCount == SubresourceRange::All || !NormalizeSubresourceRange(t, range) || !t.Usage.HasFlag(toTexture ? TextureUse::CopyDestination : TextureUse::CopySource) || !b.Usage.HasFlag(toTexture ? BufferUse::CopySource : BufferUse::CopyDestination) || t.SampleCount != 1) return Context->Reject(operation, "texture-buffer copy usage or explicit subresources are invalid");
    range = ResolveSubresourceRange(t, range);
#ifdef RADRAY_ENABLE_D3D12
    if (Context->NativeDevice->GetBackend() == RenderBackend::D3D12) {
        if (!std::has_single_bit(range.Aspects.value())) return Context->Reject(operation, "D3D12 buffer-texture copies require exactly one aspect");
        if (offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT) return Context->Reject(operation, "texture-buffer copy offset is not aligned");
        const auto* nativeTexture = static_cast<d3d12::TextureD3D12*>(Lookup(*Context, texture, operation).Get());
        const auto& nativeDesc = nativeTexture->_rawDesc;
        const auto plane = range.Aspects == TextureAspect::Stencil ? 1u : 0u;
        const auto layers = Is3D(t) ? 1u : t.DepthOrArraySize;
        uint64_t end = offset;
        for (uint32_t mip = range.BaseMipLevel; mip < range.BaseMipLevel + range.MipLevelCount; ++mip) {
            for (uint32_t layer = range.BaseArrayLayer; layer < range.BaseArrayLayer + range.ArrayLayerCount; ++layer) {
                end = Align(end, uint64_t{D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT});
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
                uint64_t bytes{};
                nativeTexture->_device->_device->GetCopyableFootprints(&nativeDesc, D3D12CalcSubresource(mip, layer, plane, t.MipLevels, layers), 1, end, &footprint, nullptr, nullptr, &bytes);
                if (bytes == UINT64_MAX || !RangeFits(b.Size, footprint.Offset, bytes)) return Context->Reject(operation, "texture-buffer copy exceeds buffer range");
                end = footprint.Offset + bytes;
            }
        }
        return true;
    }
#endif
    const auto detail = Context->NativeDevice->GetDetail();
    auto placement = std::max<uint64_t>(1, detail.TextureDataPlacementAlignment);
    auto pitch = std::max<uint64_t>(1, detail.TextureDataPitchAlignment);
    auto bpp = GetTextureFormatBytesPerPixel(t.Format);
#ifdef RADRAY_ENABLE_VULKAN
    if (Context->NativeDevice->GetBackend() == RenderBackend::Vulkan) {
        bpp = vulkan::GetTextureCopyBytesPerPixelVulkan(t.Format, range.Aspects);
        if (!bpp) return Context->Reject(operation, "Vulkan buffer-image copies require exactly one aspect");
        if (IsDepthStencilFormat(t.Format)) {
            const auto flags = static_cast<vulkan::CommandBufferVulkan*>(Real())->_cmdPool->_queue->_queueFlags;
            if (!(flags & VK_QUEUE_GRAPHICS_BIT)) return Context->Reject(operation, "Vulkan 1.2 depth/stencil copies require a graphics queue");
            placement = std::max<uint64_t>(placement, 4);
            pitch = std::max<uint64_t>(pitch, 4);
        }
    }
#endif
    if (offset % placement) return Context->Reject(operation, "texture-buffer copy offset is not aligned");
    uint64_t end = offset;
    for (uint32_t mip = range.BaseMipLevel; mip < range.BaseMipLevel + range.MipLevelCount; ++mip) {
        const uint64_t rowBytes = uint64_t(std::max(1u, t.Width >> mip)) * bpp;
        const uint64_t rowPitch = (rowBytes + pitch - 1) / pitch * pitch;
        const uint64_t rows = std::max(1u, t.Height >> mip);
        const uint64_t depth = Is3D(t) ? std::max(1u, t.DepthOrArraySize >> mip) : 1;
        if (!rowBytes || rowPitch % bpp) return Context->Reject(operation, "row pitch cannot be represented in texels");
        for (uint32_t layer = 0; layer < range.ArrayLayerCount; ++layer) {
            const uint64_t required = rowPitch * rows * depth;
            if (!RangeFits(b.Size, end, required)) return Context->Reject(operation, "texture-buffer copy exceeds buffer range");
            end += required;
        }
    }
    return true;
}

void CommandBufferValidation::CopyBufferToTexture(Texture* dst, SubresourceRange range, Buffer* src, uint64_t offset) noexcept {
    if (Recording("CopyBufferToTexture") && ValidateTextureBufferCopy(dst, range, src, offset, true, "CopyBufferToTexture")) Real()->CopyBufferToTexture(Lookup(*Context, dst, "CopyBufferToTexture").Get(), range, Lookup(*Context, src, "CopyBufferToTexture").Get(), offset);
}

void CommandBufferValidation::CopyTextureToBuffer(Buffer* dst, uint64_t offset, Texture* src, SubresourceRange range) noexcept {
    if (Recording("CopyTextureToBuffer") && ValidateTextureBufferCopy(src, range, dst, offset, false, "CopyTextureToBuffer")) Real()->CopyTextureToBuffer(Lookup(*Context, dst, "CopyTextureToBuffer").Get(), offset, Lookup(*Context, src, "CopyTextureToBuffer").Get(), range);
}

uint32_t NativeDimension(TextureDimension dim) noexcept {
    return dim == TextureDimension::Dim3D ? 3 : dim == TextureDimension::Dim1D || dim == TextureDimension::Dim1DArray ? 1
                                                                                                                      : 2;
}

void CommandBufferValidation::CopyTextureToTexture(const TextureCopyDescriptor& desc) noexcept {
    if (!Recording("CopyTextureToTexture")) return;
    const auto src = Lookup(*Context, desc.Source, "CopyTextureToTexture"), dst = Lookup(*Context, desc.Destination, "CopyTextureToTexture");
    if (!src || !dst) return;
    if (Context->NativeDevice->GetBackend() == RenderBackend::D3D12 && desc.Source == desc.Destination && desc.SourceMipLevel == desc.DestinationMipLevel && desc.SourceArrayLayer == desc.DestinationArrayLayer) {
        Context->Reject("CopyTextureToTexture", "D3D12 copies require different source and destination subresources");
        return;
    }
    const auto s = desc.Source->GetDesc(), d = desc.Destination->GetDesc();
    if (s.Format != d.Format || IsDepthStencilFormat(s.Format) || s.SampleCount != d.SampleCount || NativeDimension(s.Dim) != NativeDimension(d.Dim) || !s.Usage.HasFlag(TextureUse::CopySource) || !d.Usage.HasFlag(TextureUse::CopyDestination) || desc.SourceMipLevel >= s.MipLevels || desc.DestinationMipLevel >= d.MipLevels) {
        Context->Reject("CopyTextureToTexture", "copy format, dimension, samples, usage, or mip is invalid");
        return;
    }
    const uint32_t sw = std::max(1u, s.Width >> desc.SourceMipLevel), sh = std::max(1u, s.Height >> desc.SourceMipLevel), sd = Is3D(s) ? std::max(1u, s.DepthOrArraySize >> desc.SourceMipLevel) : 1;
    const uint32_t dw = std::max(1u, d.Width >> desc.DestinationMipLevel), dh = std::max(1u, d.Height >> desc.DestinationMipLevel), dd = Is3D(d) ? std::max(1u, d.DepthOrArraySize >> desc.DestinationMipLevel) : 1;
    if (!RangeFits(sw, desc.SourceX, desc.Width) || !RangeFits(sh, desc.SourceY, desc.Height) || !RangeFits(sd, desc.SourceZ, desc.Depth) || !RangeFits(dw, desc.DestinationX, desc.Width) || !RangeFits(dh, desc.DestinationY, desc.Height) || !RangeFits(dd, desc.DestinationZ, desc.Depth) || !RangeFits(ArrayLayers(s), desc.SourceArrayLayer, desc.ArrayLayerCount) || !RangeFits(ArrayLayers(d), desc.DestinationArrayLayer, desc.ArrayLayerCount) || (s.SampleCount > 1 && (desc.SourceX || desc.SourceY || desc.SourceZ || desc.DestinationX || desc.DestinationY || desc.DestinationZ || desc.Width != sw || desc.Height != sh || desc.Depth != sd || sw != dw || sh != dh || sd != dd))) {
        Context->Reject("CopyTextureToTexture", "copy region or layer range is invalid");
        return;
    }
    auto converted = desc;
    converted.Source = src.Get();
    converted.Destination = dst.Get();
    Real()->CopyTextureToTexture(converted);
}

void CommandBufferValidation::ResolveTexture(const TextureResolveDescriptor& desc) noexcept {
    if (!Recording("ResolveTexture")) return;
#ifdef RADRAY_ENABLE_D3D12
    if (Context->NativeDevice->GetBackend() == RenderBackend::D3D12 && Allocator->Queue->GetQueueType() != QueueType::Direct) {
        Context->Reject("ResolveTexture", "D3D12 texture resolve requires a direct command list");
        return;
    }
#endif
    const auto src = Lookup(*Context, desc.Source, "ResolveTexture"), dst = Lookup(*Context, desc.Destination, "ResolveTexture");
    if (!src || !dst) return;
    const auto s = desc.Source->GetDesc(), d = desc.Destination->GetDesc();
    if (NativeDimension(s.Dim) != 2 || NativeDimension(d.Dim) != 2 || s.Format != d.Format || IsDepthStencilFormat(s.Format) || s.SampleCount <= 1 || d.SampleCount != 1 || !s.Usage.HasFlag(TextureUse::CopySource) || !d.Usage.HasFlag(TextureUse::CopyDestination) || desc.SourceMipLevel >= s.MipLevels || desc.DestinationMipLevel >= d.MipLevels || !RangeFits(ArrayLayers(s), desc.SourceArrayLayer, desc.ArrayLayerCount) || !RangeFits(ArrayLayers(d), desc.DestinationArrayLayer, desc.ArrayLayerCount)) {
        Context->Reject("ResolveTexture", "resolve format, samples, usage, or subresources are invalid");
        return;
    }
    if (std::max(1u, s.Width >> desc.SourceMipLevel) != std::max(1u, d.Width >> desc.DestinationMipLevel) || std::max(1u, s.Height >> desc.SourceMipLevel) != std::max(1u, d.Height >> desc.DestinationMipLevel)) {
        Context->Reject("ResolveTexture", "resolve extents do not match");
        return;
    }
#ifdef RADRAY_ENABLE_D3D12
    if (Context->NativeDevice->GetBackend() == RenderBackend::D3D12) {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
        support.Format = d3d12::MapType(s.Format);
        const auto device = static_cast<d3d12::DeviceD3D12*>(Context->NativeDevice.get());
        if (FAILED(device->_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) || !(support.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE)) {
            Context->Reject("ResolveTexture", "D3D12 format does not support multisample resolve");
            return;
        }
    }
#endif
    auto converted = desc;
    converted.Source = src.Get();
    converted.Destination = dst.Get();
    Real()->ResolveTexture(converted);
}

bool CommandBufferValidation::ValidateQueryQueue(std::string_view operation, std::optional<QueryPipelineStage> stage) const noexcept {
    if (Context->NativeDevice->GetBackend() == RenderBackend::D3D12 && Allocator->Queue->GetQueueType() == QueueType::Copy)
        return Context->Reject(operation, "D3D12 regular timestamp heaps require a direct or compute command list");
#ifdef RADRAY_ENABLE_VULKAN
    if (Context->NativeDevice->GetBackend() == RenderBackend::Vulkan) {
        const auto* queue = static_cast<vulkan::CommandBufferVulkan*>(Real())->_cmdPool->_queue;
        if (stage) {
            if (!queue->_timestampValidBits || !vulkan::ArePipelineStagesSupportedVulkan(vulkan::MapType(*stage), queue->_queueFlags))
                return Context->Reject(operation, "timestamp stage or timestamps are unsupported by this Vulkan queue family");
        } else if (!(queue->_queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
            return Context->Reject(operation, "Vulkan query reset/resolve requires a graphics or compute queue");
        }
    }
#else
    RADRAY_UNUSED(operation);
    RADRAY_UNUSED(stage);
#endif
    return true;
}

void CommandBufferValidation::ResetQueryPool(QueryPool* pool, uint32_t first, uint32_t count) noexcept {
    if (!Recording("ResetQueryPool") || !ValidateQueryQueue("ResetQueryPool")) return;
    const auto native = Lookup(*Context, pool, "ResetQueryPool");
    if (!native) return;
    if (!RangeFits(pool->GetCount(), first, count)) {
        Context->Reject("ResetQueryPool", "query range is invalid");
        return;
    }
    Real()->ResetQueryPool(native.Get(), first, count);
}

void CommandBufferValidation::WriteTimestamp(const QueryTimestampDescriptor& desc) noexcept {
    if (!Recording("WriteTimestamp", false)) return;
    const auto pool = Lookup(*Context, desc.Pool, "WriteTimestamp");
    if (!pool) return;
    if (desc.Index >= desc.Pool->GetCount() || !ValidEnum(desc.Stage)) {
        Context->Reject("WriteTimestamp", "query index or stage is invalid");
        return;
    }
    if (!ValidateQueryQueue("WriteTimestamp", desc.Stage)) return;
    auto converted = desc;
    converted.Pool = pool.Get();
    Real()->WriteTimestamp(converted);
}

void CommandBufferValidation::ResolveQueryData(const QueryResolveDescriptor& desc) noexcept {
    if (!Recording("ResolveQueryData") || !ValidateQueryQueue("ResolveQueryData")) return;
    const auto pool = Lookup(*Context, desc.Pool, "ResolveQueryData");
    const auto dst = Lookup(*Context, desc.Destination, "ResolveQueryData");
    if (!pool || !dst) return;
    const auto buffer = desc.Destination->GetDesc();
    if (!RangeFits(desc.Pool->GetCount(), desc.FirstIndex, desc.Count) || desc.DestinationOffset % 8 || !buffer.Usage.HasFlag(BufferUse::CopyDestination) || !RangeFits(buffer.Size, desc.DestinationOffset, uint64_t(desc.Count) * sizeof(uint64_t))) {
        Context->Reject("ResolveQueryData", "query range, destination usage, or destination range is invalid");
        return;
    }
    auto converted = desc;
    converted.Pool = pool.Get();
    converted.Destination = dst.Get();
    Real()->ResolveQueryData(converted);
}

class SyncValidation final : public ValidatedObject<SwapChainSyncObject> {
public:
    using ValidatedObject::ValidatedObject;
};

class QueueValidation final : public ValidatedObject<CommandQueue> {
public:
    using ValidatedObject::ValidatedObject;
    QueueType GetQueueType() const noexcept override {
        if (!Ready("GetQueueType")) Context->Fatal("GetQueueType", "cannot query a destroyed queue");
        return Real()->GetQueueType();
    }
    TimestampClockCalibrationResult GetTimestampClockCalibration() const noexcept override {
        if (!Ready("GetTimestampClockCalibration")) return {.Status = TimestampClockCalibrationStatus::InvalidQueue};
        return Real()->GetTimestampClockCalibration();
    }
    void Wait() noexcept override {
        if (Ready("Wait")) Real()->Wait();
    }
    void Submit(const CommandQueueSubmitDescriptor& desc) noexcept override {
        if (!Ready("Submit")) return;
        if (desc.WaitFences.size() != desc.WaitValues.size() || desc.SignalFences.size() != desc.SignalValues.size()) {
            Context->Reject("Submit", "fence and value arrays must have matching sizes");
            return;
        }
        vector<CommandBuffer*> commands;
        for (auto* input : desc.CmdBuffers) {
            const auto wrapper = WrapperOf<CommandBufferValidation>(*Context, input, "Submit");
            if (!wrapper) return;
            if (wrapper.Get()->State != CommandBufferValidation::RecordingState::Executable || wrapper.Get()->Allocator->Queue != this) {
                Context->Reject("Submit", "commands must be closed, current recordings from this queue");
                return;
            }
            if (std::find(commands.begin(), commands.end(), wrapper.Get()->Real()) != commands.end()) {
                Context->Reject("Submit", "a command buffer occurs twice in the same submission");
                return;
            }
            commands.push_back(wrapper.Get()->Real());
        }
        vector<Fence*> waits, signals;
        for (auto* input : desc.WaitFences) {
            const auto native = Lookup(*Context, input, "Submit");
            if (!native) return;
            waits.push_back(native.Get());
        }
        for (size_t i = 0; i < desc.SignalFences.size(); ++i) {
            auto* input = desc.SignalFences[i];
            const auto native = Lookup(*Context, input, "Submit");
            if (!native) return;
            if (desc.SignalValues[i] == UINT64_MAX || desc.SignalValues[i] <= input->GetLastSignaledValue() || std::find(signals.begin(), signals.end(), native.Get()) != signals.end()) {
                Context->Reject("Submit", "fence signal values must increase and signal each fence once");
                return;
            }
            signals.push_back(native.Get());
        }
        vector<SwapChainSyncObject*> waitSync, presentSync;
        for (auto* input : desc.WaitToExecute) {
            const auto native = Lookup(*Context, input, "Submit");
            if (!native) return;
            if (std::find(waitSync.begin(), waitSync.end(), native.Get()) != waitSync.end()) {
                Context->Reject("Submit", "a swapchain wait sync occurs twice");
                return;
            }
            waitSync.push_back(native.Get());
        }
        for (auto* input : desc.ReadyToPresent) {
            const auto native = Lookup(*Context, input, "Submit");
            if (!native) return;
            if (std::find(presentSync.begin(), presentSync.end(), native.Get()) != presentSync.end()) {
                Context->Reject("Submit", "a swapchain present sync occurs twice");
                return;
            }
            presentSync.push_back(native.Get());
        }
        auto converted = desc;
        converted.CmdBuffers = commands;
        converted.WaitFences = waits;
        converted.SignalFences = signals;
        converted.WaitToExecute = waitSync;
        converted.ReadyToPresent = presentSync;
        Real()->Submit(converted);
        for (auto* command : desc.CmdBuffers) static_cast<CommandBufferValidation*>(command)->State = CommandBufferValidation::RecordingState::Submitted;
    }
};

class SwapChainValidation final : public ValidatedObject<SwapChain> {
public:
    SwapChainValidation(shared_ptr<ValidationContext> context, unique_ptr<SwapChain> native, CommandQueue* queue)
        : ValidatedObject(std::move(context), std::move(native)), Queue(queue) { AddDependency(Context->Find(queue).Get()); }
    ~SwapChainValidation() noexcept override {
        if (Live && ((NativeFrame && NativeFrame->IsValid()) || !ImagesUnused("destructor"))) Context->Fatal("swapchain destructor", "acquired frames and image references must be released first");
        Destroy();
    }
    CommandQueue* Queue;
    std::optional<SwapChainFrame> NativeFrame;
    uint64_t Token{0};
    unordered_map<Texture*, unique_ptr<TextureValidation>> Images;
    unordered_map<SwapChainSyncObject*, unique_ptr<SyncValidation>> Syncs;
    bool Alive(std::string_view operation) const noexcept {
        return (Live && Context->NativeDevice->IsValid()) || Context->Reject(operation, "swapchain or device is destroyed");
    }
    bool ImagesUnused(std::string_view operation) const noexcept {
        for (const auto& [native, image] : Images)
            if (!image->CanDestroy(operation)) return Context->Reject(operation, "swapchain image still has views or command references");
        return true;
    }
    void Destroy() noexcept override {
        if (!Live) return;
        if (NativeFrame && NativeFrame->IsValid()) {
            Context->Reject("Destroy", "swapchain still has an acquired frame");
            return;
        }
        if (!ImagesUnused("Destroy")) return;
        Images.clear();
        Syncs.clear();
        ValidatedObject::Destroy();
    }
    Nullable<SwapChainSyncObject*> WrapSync(Nullable<SwapChainSyncObject*> native) {
        if (!native) return nullptr;
        const auto found = Syncs.find(native.Get());
        if (found != Syncs.end()) return found->second.get();
        auto wrapper = make_unique<SyncValidation>(Context, native.Get());
        auto* result = wrapper.get();
        Syncs.emplace(native.Get(), std::move(wrapper));
        return result;
    }
    SwapChainAcquireResult AcquireNext(uint64_t timeout) noexcept override {
        if (!Alive("AcquireNext")) return {};
        if (NativeFrame && NativeFrame->IsValid()) {
            Context->Reject("AcquireNext", "previous acquired frame has not been consumed");
            return {};
        }
        auto result = Real()->AcquireNext(timeout);
        if (!result.Frame) return result;
        NativeFrame = std::move(result.Frame);
        Open = true;
        auto* image = NativeFrame->GetBackBuffer();
        auto found = Images.find(image);
        if (found == Images.end()) found = Images.emplace(image, make_unique<TextureValidation>(Context, image)).first;
        ++Token;
        result.Frame = MakeFrame(this, Token, found->second.get(), NativeFrame->GetBackBufferIndex(), WrapSync(NativeFrame->GetWaitToDraw()).Get(), WrapSync(NativeFrame->GetReadyToPresent()).Get());
        return result;
    }
    SwapChainPresentResult Finish(SwapChainFrame frame, bool discard) noexcept {
        const auto operation = discard ? "DiscardAcquiredFrame" : "Present";
        if (!Alive(operation)) RADRAY_ABORT("RHI validation: {}: swapchain or device is destroyed", operation);
        if (!NativeFrame || !NativeFrame->IsValid() || !ValidateFrame(frame, this, Token)) {
            Context->Fatal(operation, "frame belongs to another swapchain or has already been consumed");
            return {};
        }
        const auto result = discard ? Real()->DiscardAcquiredFrame(std::move(*NativeFrame)) : Real()->Present(std::move(*NativeFrame));
        NativeFrame.reset();
        Open = false;
        return result;
    }
    SwapChainPresentResult Present(SwapChainFrame frame) noexcept override { return Finish(std::move(frame), false); }
    SwapChainPresentResult DiscardAcquiredFrame(SwapChainFrame frame) noexcept override { return Finish(std::move(frame), true); }
    bool Recreate(uint32_t width, uint32_t height, TextureFormat format, PresentMode mode) noexcept override {
        if (!Alive("Recreate")) return false;
        if ((NativeFrame && NativeFrame->IsValid()) || !width || !height || !ValidEnum(format) || format == TextureFormat::UNKNOWN || IsDepthStencilFormat(format) || !ValidEnum(mode)) return Context->Reject("Recreate", "swapchain has an outstanding frame or invalid dimensions, format, or mode");
        if (!ImagesUnused("Recreate")) return false;
        const bool result = Real()->Recreate(width, height, format, mode);
        // A native recreate attempt can retire the old images even when it fails.
        Images.clear();
        Syncs.clear();
        return result;
    }
    uint32_t GetBackBufferCount() const noexcept override {
        if (!Alive("GetBackBufferCount")) Context->Fatal("GetBackBufferCount", "cannot query a destroyed swapchain");
        return Real()->GetBackBufferCount();
    }
    SwapChainDescriptor GetDesc() const noexcept override {
        if (!Alive("GetDesc")) Context->Fatal("GetDesc", "cannot query a destroyed swapchain");
        auto result = Real()->GetDesc();
        result.PresentQueue = Queue;
        return result;
    }
};

class DeviceValidation final : public Device {
public:
    explicit DeviceValidation(shared_ptr<ValidationContext> context) : _context(std::move(context)) { _context->Owner = this; }
    ~DeviceValidation() noexcept override {
        if (LiveChildren()) _context->Fatal("device destructor", "destroy all device objects before their device");
        _context->Owner = nullptr;
    }
    bool IsValid() const noexcept override { return _context->NativeDevice->IsValid(); }
    bool LiveChildren() const noexcept {
        std::lock_guard lock{_context->Mutex};
        return _context->LiveOwned != 0;
    }
    void Destroy() noexcept override {
        if (!IsValid()) return;
        if (LiveChildren()) {
            _context->Reject("Destroy", "device still has live owned objects");
            return;
        }
        _samplers.clear();
        _queues.clear();
        _context->NativeDevice->Destroy();
    }
    bool Ready(std::string_view operation) const noexcept { return IsValid() || _context->Reject(operation, "device is destroyed"); }
    RenderBackend GetBackend() noexcept override {
        if (!Ready("GetBackend")) _context->Fatal("GetBackend", "cannot query a destroyed device");
        return _context->NativeDevice->GetBackend();
    }
    DeviceDetail GetDetail() const noexcept override {
        if (!Ready("GetDetail")) _context->Fatal("GetDetail", "cannot query a destroyed device");
        return _context->NativeDevice->GetDetail();
    }
    const RenderDeviceCapabilities& GetCapabilities() const noexcept override {
        if (!Ready("GetCapabilities")) _context->Fatal("GetCapabilities", "cannot query a destroyed device");
        return _context->NativeDevice->GetCapabilities();
    }
    TextureSupport QueryTextureSupport(const TextureSupportQuery& query) const noexcept override {
        if (!Ready("QueryTextureSupport")) return {};
        if (!IsValidTextureSupportQuery(query)) {
            _context->Reject("QueryTextureSupport", "invalid texture support query");
            return {};
        }
        return _context->NativeDevice->QueryTextureSupport(query);
    }
    AttachmentResolveSupport QueryAttachmentResolveSupport(TextureFormat format, uint32_t samples) const noexcept override {
        if (!Ready("QueryAttachmentResolveSupport") || !ValidEnum(format) || format == TextureFormat::UNKNOWN || samples <= 1 || (samples & (samples - 1))) return {};
        return _context->NativeDevice->QueryAttachmentResolveSupport(format, samples);
    }
    Nullable<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot) noexcept override {
        if (!Ready("GetCommandQueue")) return nullptr;
        if (!ValidEnum(type) || type == QueueType::MAX_COUNT) {
            _context->Reject("GetCommandQueue", "invalid queue type");
            return nullptr;
        }
        const auto native = _context->NativeDevice->GetCommandQueue(type, slot);
        if (!native) return nullptr;
        std::lock_guard lock{_cacheMutex};
        const auto found = _queues.find(native.Get());
        if (found != _queues.end()) return found->second.get();
        auto wrapper = make_unique<QueueValidation>(_context, native.Get());
        auto* result = wrapper.get();
        _queues.emplace(native.Get(), std::move(wrapper));
        return result;
    }
    Nullable<unique_ptr<CommandAllocator>> CreateCommandAllocator(CommandQueue* queue) noexcept override {
        if (!Ready("CreateCommandAllocator")) return nullptr;
        const auto native = Lookup(*_context, queue, "CreateCommandAllocator");
        if (!native) return nullptr;
        auto result = _context->NativeDevice->CreateCommandAllocator(native.Get());
        if (!result) return nullptr;
        return make_unique<CommandAllocatorValidation>(_context, result.Release(), queue);
    }
    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandAllocator* allocator) noexcept override {
        if (!Ready("CreateCommandBuffer")) return nullptr;
        const auto wrapper = WrapperOf<CommandAllocatorValidation>(*_context, allocator, "CreateCommandBuffer");
        if (!wrapper) return nullptr;
        auto result = _context->NativeDevice->CreateCommandBuffer(wrapper.Get()->Real());
        if (!result) return nullptr;
        return make_unique<CommandBufferValidation>(_context, result.Release(), wrapper.Get());
    }
    Nullable<unique_ptr<Fence>> CreateFence() noexcept override {
        if (!Ready("CreateFence")) return nullptr;
        auto result = _context->NativeDevice->CreateFence();
        if (!result) return nullptr;
        return make_unique<FenceValidation>(_context, result.Release());
    }
    Nullable<unique_ptr<QueryPool>> CreateQueryPool(const QueryPoolDescriptor& desc) noexcept override {
        if (!Ready("CreateQueryPool")) return nullptr;
        if (!desc.Count || desc.Type != QueryType::Timestamp || !GetCapabilities().Features.TimestampQueries) {
            _context->Reject("CreateQueryPool", "invalid or unsupported query pool descriptor");
            return nullptr;
        }
        auto result = _context->NativeDevice->CreateQueryPool(desc);
        if (!result) return nullptr;
        return make_unique<QueryPoolValidation>(_context, result.Release());
    }
    Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override {
        if (!Ready("CreateSwapChain")) return nullptr;
        const auto queue = Lookup(*_context, desc.PresentQueue, "CreateSwapChain");
        if (!queue) return nullptr;
        if (!desc.NativeHandler || !desc.Width || !desc.Height || !desc.BackBufferCount || desc.Format == TextureFormat::UNKNOWN || !ValidEnum(desc.Format) || IsDepthStencilFormat(desc.Format) || !ValidEnum(desc.PresentMode) || desc.PresentQueue->GetQueueType() != QueueType::Direct) {
            _context->Reject("CreateSwapChain", "invalid swapchain dimensions, format, mode, surface, or queue");
            return nullptr;
        }
        if (GetBackend() == RenderBackend::D3D12 && (desc.BackBufferCount < 2 || desc.BackBufferCount > 16 || (desc.Format != TextureFormat::RGBA8_UNORM && desc.Format != TextureFormat::RGBA8_UNORM_SRGB && desc.Format != TextureFormat::BGRA8_UNORM && desc.Format != TextureFormat::BGRA8_UNORM_SRGB && desc.Format != TextureFormat::RGB10A2_UNORM && desc.Format != TextureFormat::RGBA16_FLOAT))) {
            _context->Reject("CreateSwapChain", "unsupported D3D12 flip-model buffer count or format");
            return nullptr;
        }
        auto converted = desc;
        converted.PresentQueue = queue.Get();
        auto result = _context->NativeDevice->CreateSwapChain(converted);
        if (!result) return nullptr;
        return make_unique<SwapChainValidation>(_context, result.Release(), desc.PresentQueue);
    }
    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override {
        if (!Ready("CreateBuffer")) return nullptr;
        if ((desc.Hints.value() & ~ResourceHints{ResourceHint::Dedicated | ResourceHint::PersistentMap}.value()) || !desc.Size || !ValidEnum(desc.Memory) || !desc.Usage || (desc.Usage.value() & ~((uint32_t(BufferUse::Indirect) << 1) - 1)) || (GetCapabilities().Limits.MaxBufferSize && desc.Size > GetCapabilities().Limits.MaxBufferSize) || (desc.Memory == MemoryType::Device && (desc.Usage.HasFlag(BufferUse::MapRead) || desc.Usage.HasFlag(BufferUse::MapWrite) || desc.Hints.HasFlag(ResourceHint::PersistentMap))) || (desc.Hints.HasFlag(ResourceHint::PersistentMap) && !desc.Usage.HasFlag(BufferUse::MapRead) && !desc.Usage.HasFlag(BufferUse::MapWrite))) {
            _context->Reject("CreateBuffer", "buffer size, usage, memory, or persistent mapping descriptor is invalid");
            return nullptr;
        }
        if ((desc.Usage.HasFlag(BufferUse::MapRead) && (desc.Usage.HasFlag(BufferUse::MapWrite) || desc.Memory != MemoryType::ReadBack)) || (desc.Usage.HasFlag(BufferUse::MapWrite) && desc.Memory != MemoryType::Upload)) {
            _context->Reject("CreateBuffer", "MapRead requires readback memory and MapWrite requires upload memory");
            return nullptr;
        }
        if (GetBackend() == RenderBackend::D3D12 && !desc.Usage.HasFlag(BufferUse::UnorderedAccess) &&
            ((desc.Memory == MemoryType::Upload && desc.Usage.HasFlag(BufferUse::CopyDestination)) ||
             (desc.Memory == MemoryType::ReadBack && (desc.Usage & ~(BufferUse::MapRead | BufferUse::CopyDestination))))) {
            _context->Reject("CreateBuffer", "D3D12 upload/readback heap usage is incompatible with its fixed resource state");
            return nullptr;
        }
        auto result = _context->NativeDevice->CreateBuffer(desc);
        if (!result) return nullptr;
        return make_unique<BufferValidation>(_context, result.Release());
    }
    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override {
        if (!Ready("CreateTexture")) return nullptr;
        const auto [valid, reason] = ValidateTextureDescriptor(desc, *this);
        if (!valid) {
            _context->Reject("CreateTexture", reason);
            return nullptr;
        }
        auto result = _context->NativeDevice->CreateTexture(desc);
        if (!result) return nullptr;
        return make_unique<TextureValidation>(_context, result.Release());
    }
    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override {
        if (!Ready("CreateTextureView")) return nullptr;
        const auto texture = Lookup(*_context, desc.Target, "CreateTextureView");
        if (!texture) return nullptr;
        const auto target = desc.Target->GetDesc();
        auto range = desc.Range;
        if ((desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim2D || desc.Dim == TextureDimension::Dim3D) && range.ArrayLayerCount == SubresourceRange::All) range.ArrayLayerCount = 1;
        if (!range.Aspects && desc.Usage == TextureViewUsage::Resource && IsDepthStencilFormat(target.Format)) range.Aspects = TextureAspect::Depth;
        if (!ValidEnum(desc.Dim) || desc.Dim == TextureDimension::UNKNOWN || !ValidEnum(desc.Format) || desc.Format == TextureFormat::UNKNOWN || !ValidEnum(desc.Usage) || desc.Usage == TextureViewUsage::UNKNOWN) {
            _context->Reject("CreateTextureView", "invalid view enum");
            return nullptr;
        }
        const auto normalized = NormalizeSubresourceRange(target, range);
        if (!normalized || !ValidEnum(desc.Dim) || desc.Dim == TextureDimension::UNKNOWN || NativeDimension(desc.Dim) != NativeDimension(target.Dim) || !ValidEnum(desc.Format) || desc.Format == TextureFormat::UNKNOWN || !ValidEnum(desc.Usage) || desc.Usage == TextureViewUsage::UNKNOWN) {
            _context->Reject("CreateTextureView", "invalid texture view dimension, format, usage, or range");
            return nullptr;
        }
        const auto count = normalized->ArrayLayerCount;
        const bool nonArrayView = desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim2D || desc.Dim == TextureDimension::Dim3D;
        const bool cubeView = desc.Dim == TextureDimension::Cube || desc.Dim == TextureDimension::CubeArray;
        if ((nonArrayView && count != 1) || (desc.Dim == TextureDimension::Dim3D && normalized->BaseArrayLayer) || (desc.Dim == TextureDimension::Cube && count != 6) || (desc.Dim == TextureDimension::CubeArray && (normalized->BaseArrayLayer % 6 || count % 6))) {
            _context->Reject("CreateTextureView", "view dimension does not match its layer range");
            return nullptr;
        }
        if (GetBackend() == RenderBackend::D3D12 && normalized->BaseArrayLayer && (nonArrayView || desc.Dim == TextureDimension::Cube)) {
            _context->Reject("CreateTextureView", "D3D12 non-array views cannot select a nonzero base array layer");
            return nullptr;
        }
        if (GetBackend() == RenderBackend::Vulkan && cubeView && target.Dim != TextureDimension::Cube && target.Dim != TextureDimension::CubeArray) {
            _context->Reject("CreateTextureView", "Vulkan cube views require a cube-compatible texture dimension");
            return nullptr;
        }
        const auto usage = desc.Usage == TextureViewUsage::Resource ? TextureUse::Resource : desc.Usage == TextureViewUsage::RenderTarget ? TextureUse::RenderTarget
                                                                                         : desc.Usage == TextureViewUsage::DepthRead      ? TextureUse::DepthStencilRead
                                                                                         : IsDepthStencilViewUsage(desc.Usage)            ? TextureUse::DepthStencilWrite
                                                                                                                                          : TextureUse::UnorderedAccess;
        const bool depth = IsDepthStencilViewUsage(desc.Usage);
        const bool mixed = desc.Usage == TextureViewUsage::DepthReadStencilWrite || desc.Usage == TextureViewUsage::DepthWriteStencilRead;
        if (mixed && (!GetTextureFormatAspects(target.Format).HasFlag(TextureAspect::Stencil) || !target.Usage.HasFlag(TextureUse::DepthStencilRead) || !target.Usage.HasFlag(TextureUse::DepthStencilWrite))) {
            _context->Reject("CreateTextureView", "mixed permissions require depth/stencil format and read/write usage");
            return nullptr;
        }
        if (!target.Usage.HasFlag(usage) || (depth && (!IsDepthStencilFormat(desc.Format) || normalized->Aspects != GetTextureFormatAspects(desc.Format))) || (desc.Usage == TextureViewUsage::Resource && normalized->Aspects.HasFlag(TextureAspect::Depth) && normalized->Aspects.HasFlag(TextureAspect::Stencil)) || (desc.Usage == TextureViewUsage::UnorderedAccess && (target.SampleCount != 1 || IsDepthStencilFormat(desc.Format)))) {
            _context->Reject("CreateTextureView", "view usage or aspects are incompatible with the texture");
            return nullptr;
        }
        auto converted = desc;
        converted.Target = texture.Get();
        converted.Range = *normalized;
        std::pair<bool, string> support{false, "texture view backend is unavailable"};
#if defined(RADRAY_ENABLE_D3D12)
        if (GetBackend() == RenderBackend::D3D12)
            support = static_cast<d3d12::DeviceD3D12*>(_context->NativeDevice.get())->ValidateTextureView(converted);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
        if (GetBackend() == RenderBackend::Vulkan)
            support = static_cast<vulkan::DeviceVulkan*>(_context->NativeDevice.get())->ValidateTextureView(converted);
#endif
        if (!support.first) {
            _context->Reject("CreateTextureView", support.second);
            return nullptr;
        }
        auto result = _context->NativeDevice->CreateTextureView(converted);
        if (!result) return nullptr;
        return make_unique<TextureViewValidation>(_context, result.Release(), desc.Target);
    }
    Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override {
        if (!Ready("CreateShader")) return nullptr;
        const auto expected = GetBackend() == RenderBackend::D3D12 ? ShaderBlobCategory::DXIL : ShaderBlobCategory::SPIRV;
        if (desc.Source.empty() || desc.Category != expected || !desc.Stages || (desc.Stages.value() & ~ShaderStages{ShaderStage::Graphics | ShaderStage::Compute}.value()) || (expected == ShaderBlobCategory::SPIRV && desc.Source.size() % 4)) {
            _context->Reject("CreateShader", "shader source, target, stages, or SPIR-V size is invalid");
            return nullptr;
        }
        auto result = _context->NativeDevice->CreateShader(desc);
        if (!result) return nullptr;
        return make_unique<ShaderValidation>(_context, result.Release());
    }
    Nullable<unique_ptr<BindingGroupLayout>> CreateBindingGroupLayout(const BindingGroupLayoutDescriptor& desc) noexcept override {
        if (!Ready("CreateBindingGroupLayout")) return nullptr;
        const bool dx = GetBackend() == RenderBackend::D3D12;
        unordered_set<uint32_t> ids;
        std::optional<uint32_t> vkSet;
        for (const auto& binding : desc.Bindings) {
            if (!ValidEnum(binding.Kind) || !binding.Count || !binding.Stages ||
                (binding.Stages & ~ShaderStages{ShaderStage::Graphics | ShaderStage::Compute}) || !ids.insert(binding.BindingId).second ||
                (dx ? !binding.D3D12.has_value() : !binding.Vulkan.has_value())) {
                _context->Reject("CreateBindingGroupLayout", "invalid kind, count, stages, duplicate ID or missing mapping");
                return nullptr;
            }
            bool dynamic;
            if (dx) {
                const auto& native = *binding.D3D12;
                if (!ValidEnum(native.Placement) || native.RegisterType != GetBindingRegisterClass(binding.Kind) || binding.Count - 1 > UINT32_MAX - native.Register) {
                    _context->Reject("CreateBindingGroupLayout", "invalid D3D12 register class, placement or range");
                    return nullptr;
                }
                dynamic = native.Placement == D3D12BufferPlacement::RootDescriptor;
            } else {
                const auto& native = *binding.Vulkan;
                if (!ValidEnum(native.Placement) || native.Set == UINT32_MAX || (vkSet && *vkSet != native.Set)) {
                    _context->Reject("CreateBindingGroupLayout", "group must occupy one Vulkan set with valid placement");
                    return nullptr;
                }
                vkSet = native.Set;
                dynamic = native.Placement == VulkanBufferDescriptorPlacement::Dynamic;
            }
            if ((dynamic && (binding.Count != 1 || !CanBeRootDescriptor(binding.Kind))) ||
                (binding.ImmutableSampler && (binding.Kind != BindingKind::Sampler || binding.Count != 1 || !ValidateSampler(*binding.ImmutableSampler)))) {
                _context->Reject("CreateBindingGroupLayout", "invalid dynamic buffer or immutable sampler");
                return nullptr;
            }
        }
        // Native coordinates must be unambiguous inside a group. This is creation validity, not cross-layout compatibility.
        for (size_t i = 0; i < desc.Bindings.size(); ++i)
            for (size_t j = 0; j < i; ++j) {
                const auto& a = desc.Bindings[i];
                const auto& b = desc.Bindings[j];
                const bool overlap = dx ? (a.D3D12->Space == b.D3D12->Space && a.D3D12->RegisterType == b.D3D12->RegisterType &&
                                           uint64_t(a.D3D12->Register) < uint64_t(b.D3D12->Register) + b.Count && uint64_t(b.D3D12->Register) < uint64_t(a.D3D12->Register) + a.Count)
                                        : a.Vulkan->Binding == b.Vulkan->Binding;
                if (overlap) {
                    _context->Reject("CreateBindingGroupLayout", "native binding locations overlap");
                    return nullptr;
                }
            }
        auto native = _context->NativeDevice->CreateBindingGroupLayout(desc);
        if (!native) return nullptr;
        auto result = make_unique<BindingGroupLayoutValidation>(_context, native.Release());
        result->Descriptors.assign(desc.Bindings.begin(), desc.Bindings.end());
        for (const auto& binding : desc.Bindings) result->Bindings.push_back({binding.BindingId, binding.Count, binding.Kind,
                                                                              dx ? binding.D3D12->Placement == D3D12BufferPlacement::RootDescriptor : binding.Vulkan->Placement == VulkanBufferDescriptorPlacement::Dynamic,
                                                                              binding.ImmutableSampler.has_value()});
        return result;
    }
    Nullable<unique_ptr<PipelineLayout>> CreatePipelineLayout(const PipelineLayoutDescriptor& desc) noexcept override {
        if (!Ready("CreatePipelineLayout")) return nullptr;
        vector<PipelineLayoutGroupDescriptor> converted;
        vector<BindingGroupLayoutValidation*> dependencies;
        unordered_set<uint32_t> slots, pushIds;
        const bool dx = GetBackend() == RenderBackend::D3D12;
        for (const auto& group : desc.Groups) {
            const auto layout = WrapperOf<BindingGroupLayoutValidation>(*_context, group.Layout, "CreatePipelineLayout");
            if (!layout) return nullptr;
            if (!slots.insert(group.GroupSlot).second) {
                _context->Reject("CreatePipelineLayout", "duplicate group slot");
                return nullptr;
            }
            converted.push_back({group.GroupSlot, layout.Get()->Real()});
            dependencies.push_back(layout.Get());
        }
        for (const auto& push : desc.PushConstants) {
            if (!pushIds.insert(push.PushId).second || !push.Size || push.Size % 4 || !push.Stages ||
                (push.Stages & ~ShaderStages{ShaderStage::Graphics | ShaderStage::Compute}) ||
                (dx ? !push.D3D12.has_value() : (!push.Vulkan || push.Vulkan->Offset % 4 || uint64_t(push.Vulkan->Offset) + push.Size > GetCapabilities().Limits.MaxPushConstantBytes))) {
                _context->Reject("CreatePipelineLayout", "invalid push constant ID, mapping, stages or range");
                return nullptr;
            }
        }
        if (!dx && desc.PushConstants.size() > 1) {
            _context->Reject("CreatePipelineLayout", "Vulkan supports one push block");
            return nullptr;
        }
#ifdef RADRAY_ENABLE_VULKAN
        if (!dx && !ValidateLayoutLimitsVulkan(*_context, *static_cast<vulkan::DeviceVulkan*>(_context->NativeDevice.get()), desc)) return nullptr;
#endif
        auto native = _context->NativeDevice->CreatePipelineLayout({converted, desc.PushConstants});
        if (!native) return nullptr;
        auto result = make_unique<PipelineLayoutValidation>(_context, native.Release());
        result->Groups.assign(desc.Groups.begin(), desc.Groups.end());
        result->Pushes.assign(desc.PushConstants.begin(), desc.PushConstants.end());
        for (auto* dependency : dependencies) result->AddDependency(dependency);
        return result;
    }
    Nullable<unique_ptr<ShaderParameterSet>> CreateShaderParameterSet(const ShaderParameterSetDescriptor& desc) noexcept override {
        if (!Ready("CreateShaderParameterSet")) return nullptr;
        const auto layout = WrapperOf<BindingGroupLayoutValidation>(*_context, desc.Layout, "CreateShaderParameterSet");
        if (!layout) return nullptr;
        auto native = _context->NativeDevice->CreateShaderParameterSet({layout.Get()->Real()});
        if (!native) return nullptr;
        return make_unique<ShaderParameterSetValidation>(_context, native.Release(), layout.Get());
    }
    Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override {
        if (!Ready("CreateGraphicsPipelineState")) return nullptr;
        const auto layout = WrapperOf<PipelineLayoutValidation>(*_context, desc.PipelineLayout, "CreateGraphicsPipelineState");
        if (!layout) return nullptr;
        for (const auto& attribute : desc.VertexInput.Attributes)
            if (!ValidEnum(attribute.Format)) {
                _context->Reject("CreateGraphicsPipelineState", "invalid vertex attribute format");
                return nullptr;
            }
        for (const auto& buffer : desc.VertexInput.Buffers)
            if (!ValidEnum(buffer.StepMode)) {
                _context->Reject("CreateGraphicsPipelineState", "invalid vertex step mode");
                return nullptr;
            }
        if (!desc.VS || !ValidateVertexInputState(desc.VertexInput) || !ValidEnum(desc.Primitive.Topology) || !ValidEnum(desc.Primitive.Cull) || !ValidEnum(desc.Primitive.FaceClockwise) || !ValidEnum(desc.Primitive.Poly) || (desc.Primitive.StripIndexFormat && (desc.Primitive.Topology != PrimitiveTopology::TriangleStrip && desc.Primitive.Topology != PrimitiveTopology::LineStrip)) || desc.ColorTargets.size() > GetCapabilities().Limits.MaxColorAttachments || (!std::has_single_bit(desc.MultiSample.Count) || desc.MultiSample.Count > 64)) {
            _context->Reject("CreateGraphicsPipelineState", "invalid vertex input, primitive, attachment count, or sample count");
            return nullptr;
        }
        if (desc.Primitive.StripIndexFormat && !ValidEnum(*desc.Primitive.StripIndexFormat)) {
            _context->Reject("CreateGraphicsPipelineState", "invalid strip index format");
            return nullptr;
        }
        if (GetBackend() == RenderBackend::D3D12 && desc.Primitive.Poly == PolygonMode::Point) {
            _context->Reject("CreateGraphicsPipelineState", "D3D12 does not support point polygon mode");
            return nullptr;
        }
        for (const auto& target : desc.ColorTargets) {
            if (target.Blend) {
                const auto valid = [](const BlendComponent& component) { return ValidEnum(component.Src) && ValidEnum(component.Dst) && ValidEnum(component.Op); };
                if (!valid(target.Blend->Color) || !valid(target.Blend->Alpha)) {
                    _context->Reject("CreateGraphicsPipelineState", "invalid blend state");
                    return nullptr;
                }
            }
        }
        if (desc.DepthStencil) {
            const auto& state = *desc.DepthStencil;
            if (!ValidEnum(state.Format) || !ValidEnum(state.DepthCompare) || !std::isfinite(state.DepthBias.Clamp) || !std::isfinite(state.DepthBias.SlopScale)) {
                _context->Reject("CreateGraphicsPipelineState", "invalid depth state");
                return nullptr;
            }
            if (state.Stencil) {
                const auto valid = [](const StencilFaceState& face) { return ValidEnum(face.Compare) && ValidEnum(face.FailOp) && ValidEnum(face.DepthFailOp) && ValidEnum(face.PassOp); };
                if (!GetTextureFormatAspects(state.Format).HasFlag(TextureAspect::Stencil) || !valid(state.Stencil->Front) || !valid(state.Stencil->Back) || (state.Stencil->ReadMask | state.Stencil->WriteMask) > 255) {
                    _context->Reject("CreateGraphicsPipelineState", "invalid stencil format, operations, or mask");
                    return nullptr;
                }
            }
        }
        auto converted = desc;
        converted.PipelineLayout = layout.Get()->Real();
        const auto shaderEntry = [&](std::optional<ShaderEntry>& entry, ShaderStage stage) {
            if (!entry) return true;
            const auto native = Lookup(*_context, entry->Target, "CreateGraphicsPipelineState");
            if (!native) return false;
            if (entry->EntryPoint.empty() || !entry->Target->GetStages().HasFlag(stage)) return _context->Reject("CreateGraphicsPipelineState", "shader entry has an empty name or incompatible stage");
            entry->Target = native.Get();
            return true;
        };
        if (!shaderEntry(converted.VS, ShaderStage::Vertex) || !shaderEntry(converted.PS, ShaderStage::Pixel)) return nullptr;
        for (const auto& buffer : desc.VertexInput.Buffers)
            if (buffer.Binding >= GetDetail().MaxVertexInputBindings) {
                _context->Reject("CreateGraphicsPipelineState", "vertex input slot exceeds device limits");
                return nullptr;
            }
        for (const auto& target : desc.ColorTargets)
            if (!ValidEnum(target.Format) || target.Format == TextureFormat::UNKNOWN || IsDepthStencilFormat(target.Format) || (target.WriteMask.value() & ~uint32_t(ColorWrite::All))) {
                _context->Reject("CreateGraphicsPipelineState", "invalid color target format or write mask");
                return nullptr;
            }
        if (desc.DepthStencil && !IsDepthStencilFormat(desc.DepthStencil->Format)) {
            _context->Reject("CreateGraphicsPipelineState", "depth attachment requires a depth format");
            return nullptr;
        }
#ifdef RADRAY_ENABLE_VULKAN
        if (auto* native = dynamic_cast<vulkan::DeviceVulkan*>(_context->NativeDevice.get())) {
            if (const auto unsupported = vulkan::GetUnsupportedGraphicsStateVulkan(desc, native->_enabledFeatures, native->_extProperties.conservativeRasterization.has_value())) {
                _context->Reject("CreateGraphicsPipelineState", *unsupported);
                return nullptr;
            }
        }
#endif
        auto result = _context->NativeDevice->CreateGraphicsPipelineState(converted);
        if (!result) return nullptr;
        return make_unique<GraphicsPipelineValidation>(_context, result.Release(), layout.Get(), desc);
    }
    Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept override {
        if (!Ready("CreateComputePipelineState")) return nullptr;
        const auto layout = WrapperOf<PipelineLayoutValidation>(*_context, desc.PipelineLayout, "CreateComputePipelineState");
        const auto shader = Lookup(*_context, desc.CS.Target, "CreateComputePipelineState");
        if (!layout || !shader) return nullptr;
        if (desc.CS.EntryPoint.empty() || !desc.CS.Target->GetStages().HasFlag(ShaderStage::Compute)) {
            _context->Reject("CreateComputePipelineState", "compute shader name or stage is invalid");
            return nullptr;
        }
        auto converted = desc;
        converted.PipelineLayout = layout.Get()->Real();
        converted.CS.Target = shader.Get();
        auto result = _context->NativeDevice->CreateComputePipelineState(converted);
        if (!result) return nullptr;
        return make_unique<ComputePipelineValidation>(_context, result.Release(), layout.Get());
    }
    bool ValidateSampler(const SamplerDescriptor& desc) const noexcept {
        if (!ValidEnum(desc.AddressS) || !ValidEnum(desc.AddressT) || !ValidEnum(desc.AddressR) ||
            !ValidEnum(desc.MinFilter) || !ValidEnum(desc.MagFilter) || !ValidEnum(desc.MipmapFilter) ||
            !ValidEnum(desc.BorderColor) || !ValidEnum(desc.Reduction) || (desc.Compare && !ValidEnum(*desc.Compare)) ||
            !std::isfinite(desc.LodMin) || !std::isfinite(desc.LodMax) || !std::isfinite(desc.MipLodBias) ||
            desc.LodMin > desc.LodMax || desc.AnisotropyClamp > 16 ||
            (desc.Compare && desc.Reduction != SamplerReductionMode::WeightedAverage))
            return _context->Reject("CreateSampler", "sampler modes, LOD range, comparison reduction or anisotropy are invalid");
        if (desc.UnnormalizedCoordinates && (desc.MinFilter != desc.MagFilter || desc.MipmapFilter != FilterMode::Nearest ||
                                             desc.LodMin != 0 || desc.LodMax != 0 || desc.AnisotropyClamp > 1 || desc.Compare ||
                                             (desc.AddressS != AddressMode::ClampToEdge && desc.AddressS != AddressMode::ClampToBorder) ||
                                             (desc.AddressT != AddressMode::ClampToEdge && desc.AddressT != AddressMode::ClampToBorder)))
            return _context->Reject("CreateSampler", "invalid unnormalized sampler configuration");
        const auto& caps = _context->NativeDevice->GetCapabilities();
        const auto& features = caps.Features;
        if (desc.MipLodBias < caps.Limits.MinSamplerLodBias || desc.MipLodBias > caps.Limits.MaxSamplerLodBias ||
            (desc.AnisotropyClamp > 1 && (!features.SamplerAnisotropy || desc.AnisotropyClamp > caps.Limits.MaxSamplerAnisotropy)) ||
            (desc.AnisotropyClamp > 1 && desc.MipmapFilter == FilterMode::Nearest && !features.SamplerAnisotropyPointMip) ||
            (desc.Reduction != SamplerReductionMode::WeightedAverage && !features.SamplerFilterMinmax) ||
            (desc.UnnormalizedCoordinates && !features.SamplerUnnormalizedCoordinates) ||
            ((static_cast<uint32_t>(desc.BorderColor) & 1u) && !features.SamplerIntegerBorderColor) ||
            ((desc.AddressS == AddressMode::MirrorClampToEdge || desc.AddressT == AddressMode::MirrorClampToEdge || desc.AddressR == AddressMode::MirrorClampToEdge) && !features.SamplerMirrorClampToEdge))
            return _context->Reject("CreateSampler", "sampler exceeds backend device capabilities");

        return true;
    }
    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc) noexcept override {
        if (!Ready("CreateSampler") || !ValidateSampler(desc)) return nullptr;
        auto result = _context->NativeDevice->CreateSampler(desc);
        if (!result) return nullptr;
        return make_unique<SamplerValidation>(_context, result.Release());
    }
    Nullable<Sampler*> GetOrCreateSampler(const SamplerDescriptor& desc) noexcept override {
        if (!Ready("GetOrCreateSampler") || !ValidateSampler(desc)) return nullptr;
        std::lock_guard lock{_cacheMutex};
        const auto native = _context->NativeDevice->GetOrCreateSampler(desc);
        if (!native) return nullptr;
        const auto found = _samplers.find(native.Get());
        if (found != _samplers.end()) return found->second.get();
        auto result = make_unique<SamplerValidation>(_context, native.Get());
        auto* pointer = result.get();
        _samplers.emplace(native.Get(), std::move(result));
        return pointer;
    }

private:
    shared_ptr<ValidationContext> _context;
    std::mutex _cacheMutex;
    unordered_map<CommandQueue*, unique_ptr<QueueValidation>> _queues;
    unordered_map<Sampler*, unique_ptr<SamplerValidation>> _samplers;
};

}  // namespace

Nullable<shared_ptr<Device>> CreateValidationDevice(shared_ptr<Device> device, const RhiValidationOptions& options) {
    auto context = make_shared<ValidationContext>();
    context->NativeDevice = std::move(device);
    context->Options = options;
    return make_shared<DeviceValidation>(std::move(context));
}

}  // namespace radray::render::detail
