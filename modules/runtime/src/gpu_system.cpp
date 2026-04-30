#include <radray/runtime/gpu_system.h>

#include <algorithm>
#include <type_traits>
#include <variant>

#include <radray/logger.h>
#include <radray/sparse_set.h>
#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

namespace radray {

GpuResourceRegistry::ParentRef::ParentRef(GpuResourceRegistry* registry, GpuResourceHandle handle) noexcept
    : Registry(registry),
      Handle(handle) {}

template <typename Record>
GpuResourceRegistry::ResourceRecord::ResourceRecord(
    Record record,
    void* nativeHandle,
    vector<GpuResourceRegistry::ParentRef> parentRefs) noexcept
    : Data(std::move(record)),
      NativeHandle(nativeHandle),
      ParentRefs(std::move(parentRefs)) {}

GpuResourceRegistry::Kind GpuResourceRegistry::ResourceRecord::GetKind() const noexcept {
    return std::visit(
        [](const auto& record) noexcept {
            using Record = std::decay_t<decltype(record)>;
            if constexpr (std::is_same_v<Record, BufferRecord>) {
                return Kind::Buffer;
            } else if constexpr (std::is_same_v<Record, TextureRecord>) {
                return Kind::Texture;
            } else if constexpr (std::is_same_v<Record, TextureViewRecord>) {
                return Kind::TextureView;
            } else if constexpr (std::is_same_v<Record, SamplerRecord>) {
                return Kind::Sampler;
            } else if constexpr (std::is_same_v<Record, ShaderRecord>) {
                return Kind::Shader;
            } else if constexpr (std::is_same_v<Record, RootSignatureRecord>) {
                return Kind::RootSignature;
            } else if constexpr (std::is_same_v<Record, DescriptorSetRecord>) {
                return Kind::DescriptorSet;
            } else if constexpr (std::is_same_v<Record, GraphicsPipelineStateRecord>) {
                return Kind::GraphicsPipelineState;
            } else if constexpr (std::is_same_v<Record, ComputePipelineStateRecord>) {
                return Kind::ComputePipelineState;
            } else if constexpr (std::is_same_v<Record, AccelerationStructureRecord>) {
                return Kind::AccelerationStructure;
            } else if constexpr (std::is_same_v<Record, AccelerationStructureViewRecord>) {
                return Kind::AccelerationStructureView;
            } else if constexpr (std::is_same_v<Record, RayTracingPipelineStateRecord>) {
                return Kind::RayTracingPipelineState;
            } else if constexpr (std::is_same_v<Record, ShaderBindingTableRecord>) {
                return Kind::ShaderBindingTable;
            } else {
                static_assert(std::is_same_v<Record, BindlessArrayRecord>);
                return Kind::BindlessArray;
            }
        },
        Data);
}

SparseSetHandle GpuResourceRegistry::ToRecordHandle(const GpuResourceHandle& handle) noexcept {
    return SparseSetHandle{handle.Handle, handle.Generation};
}

GpuTask::GpuTask(GpuRuntime* runtime, render::Fence* fence, uint64_t signalValue) noexcept
    : _runtime(runtime),
      _fence(fence),
      _signalValue(signalValue) {}

bool GpuTask::IsValid() const {
    return _fence != nullptr && _signalValue != 0;
}

bool GpuTask::IsCompleted() const {
    return !this->IsValid() || _fence->GetCompletedValue() >= _signalValue;
}

void GpuTask::Wait() const {
    if (this->IsValid()) {
        _fence->Wait(_signalValue);
    }
}

GpuSurface::GpuSurface(
    GpuRuntime* runtime,
    unique_ptr<render::SwapChain> swapchain,
    uint32_t queueSlot) noexcept
    : _runtime(runtime),
      _swapchain(std::move(swapchain)),
      _queueSlot(queueSlot) {}

GpuSurface::~GpuSurface() noexcept {
    this->Destroy();
}

bool GpuSurface::IsValid() const {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        return _swapchain != nullptr;
    }
    return _swapchain != nullptr;
}

void GpuSurface::Destroy() {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        _swapchain.reset();
        return;
    }
    _swapchain.reset();
}

GpuResourceRegistry::GpuResourceRegistry(render::Device* device) noexcept
    : _device(device) {}

GpuResourceRegistry::~GpuResourceRegistry() noexcept {
    this->Clear();
}

template <typename Handle, typename Record>
Handle GpuResourceRegistry::AddRecord(Record resourceRecord, void* nativeHandle, vector<ParentRef> parentRefs) {
    RADRAY_ASSERT(nativeHandle != nullptr);
    const SparseSetHandle recordHandle = _records.Emplace(ResourceRecord{std::move(resourceRecord), nativeHandle, std::move(parentRefs)});
    ResourceRecord& record = _records.Get(recordHandle);
    record.RecordHandle = recordHandle;
    for (const ParentRef& parent : record.ParentRefs) {
        RADRAY_ASSERT(parent.Registry != nullptr);
        parent.Registry->AddChildRef(parent.Handle);
    }
    return {recordHandle.Index, recordHandle.Generation, nativeHandle};
}

template <typename Record, typename Native>
Native* GpuResourceRegistry::FindAliveAs(const GpuResourceHandle& handle) noexcept {
    auto* record = this->FindRecord(handle);
    if (record == nullptr || record->State != State::Alive) {
        return nullptr;
    }
    auto* typedRecord = std::get_if<Record>(&record->Data);
    return typedRecord != nullptr ? typedRecord->Resource.get() : nullptr;
}

template <typename Record, typename Native>
const Native* GpuResourceRegistry::FindAliveAs(const GpuResourceHandle& handle) const noexcept {
    const auto* record = this->FindRecord(handle);
    if (record == nullptr || record->State != State::Alive) {
        return nullptr;
    }
    const auto* typedRecord = std::get_if<Record>(&record->Data);
    return typedRecord != nullptr ? typedRecord->Resource.get() : nullptr;
}

GpuBufferHandle GpuResourceRegistry::CreateBuffer(const render::BufferDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto bufferOpt = _device->CreateBuffer(desc);
    if (!bufferOpt.HasValue()) {
        throw GpuSystemException("Device::CreateBuffer failed");
    }

    auto buffer = bufferOpt.Release();
    auto* native = buffer.get();
    return this->AddRecord<GpuBufferHandle>(BufferRecord{std::move(buffer)}, native);
}

GpuTextureHandle GpuResourceRegistry::CreateTexture(const render::TextureDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto textureOpt = _device->CreateTexture(desc);
    if (!textureOpt.HasValue()) {
        throw GpuSystemException("Device::CreateTexture failed");
    }

    auto texture = textureOpt.Release();
    auto* native = texture.get();
    return this->AddRecord<GpuTextureHandle>(TextureRecord{std::move(texture)}, native);
}

GpuTextureViewHandle GpuResourceRegistry::CreateTextureView(const GpuTextureViewDescriptor& desc, const ParentResourceRef& parent) {
    RADRAY_ASSERT(_device != nullptr);
    RADRAY_ASSERT(parent.Registry != nullptr);
    RADRAY_ASSERT(parent.Handle.IsValid());
    auto* parentTexture = static_cast<render::Texture*>(parent.NativeHandle);
    RADRAY_ASSERT(parentTexture != nullptr);

    render::TextureViewDescriptor nativeDesc{};
    nativeDesc.Target = parentTexture;
    nativeDesc.Dim = desc.Dim;
    nativeDesc.Format = desc.Format;
    nativeDesc.Range = desc.Range;
    nativeDesc.Usage = desc.Usage;

    auto textureViewOpt = _device->CreateTextureView(nativeDesc);
    if (!textureViewOpt.HasValue()) {
        throw GpuSystemException("Device::CreateTextureView failed");
    }

    auto textureView = textureViewOpt.Release();
    auto* native = textureView.get();
    return this->AddRecord<GpuTextureViewHandle>(
        TextureViewRecord{std::move(textureView)},
        native,
        vector<ParentRef>{{parent.Registry, parent.Handle}});
}

GpuSamplerHandle GpuResourceRegistry::CreateSampler(const render::SamplerDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto samplerOpt = _device->CreateSampler(desc);
    if (!samplerOpt.HasValue()) {
        throw GpuSystemException("Device::CreateSampler failed");
    }

    auto sampler = samplerOpt.Release();
    auto* native = sampler.get();
    return this->AddRecord<GpuSamplerHandle>(SamplerRecord{std::move(sampler)}, native);
}

GpuShaderHandle GpuResourceRegistry::CreateShader(const render::ShaderDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto shaderOpt = _device->CreateShader(desc);
    if (!shaderOpt.HasValue()) {
        throw GpuSystemException("Device::CreateShader failed");
    }

    auto shader = shaderOpt.Release();
    auto* native = shader.get();
    return this->AddRecord<GpuShaderHandle>(ShaderRecord{std::move(shader)}, native);
}

GpuRootSignatureHandle GpuResourceRegistry::CreateRootSignature(const GpuRootSignatureDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);

    vector<render::Shader*> shaders{};
    vector<ParentRef> parentRefs{};
    shaders.reserve(desc.Shaders.size());
    parentRefs.reserve(desc.Shaders.size());
    for (const GpuShaderHandle& shaderHandle : desc.Shaders) {
        auto* shader = this->FindAliveShader(shaderHandle);
        RADRAY_ASSERT(shader != nullptr);
        shaders.emplace_back(shader);
        parentRefs.emplace_back(this, shaderHandle);
    }

    render::RootSignatureDescriptor nativeDesc{};
    nativeDesc.Shaders = std::span<render::Shader*>{shaders.data(), shaders.size()};
    nativeDesc.StaticSamplers = desc.StaticSamplers;
    auto rootSigOpt = _device->CreateRootSignature(nativeDesc);
    if (!rootSigOpt.HasValue()) {
        throw GpuSystemException("Device::CreateRootSignature failed");
    }

    auto rootSig = rootSigOpt.Release();
    auto* native = rootSig.get();
    return this->AddRecord<GpuRootSignatureHandle>(
        RootSignatureRecord{std::move(rootSig)},
        native,
        std::move(parentRefs));
}

GpuDescriptorSetHandle GpuResourceRegistry::CreateDescriptorSet(const GpuDescriptorSetDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto* rootSig = this->FindAliveRootSignature(desc.RootSig);
    RADRAY_ASSERT(rootSig != nullptr);

    auto descriptorSetOpt = _device->CreateDescriptorSet(rootSig, desc.Set);
    if (!descriptorSetOpt.HasValue()) {
        throw GpuSystemException("Device::CreateDescriptorSet failed");
    }

    auto descriptorSet = descriptorSetOpt.Release();
    auto* native = descriptorSet.get();
    return this->AddRecord<GpuDescriptorSetHandle>(
        DescriptorSetRecord{std::move(descriptorSet)},
        native,
        vector<ParentRef>{{this, desc.RootSig}});
}

render::ShaderEntry GpuResourceRegistry::MakeNativeShaderEntry(const GpuShaderEntry& entry, vector<ParentRef>& parentRefs) {
    auto* shader = this->FindAliveShader(entry.Target);
    RADRAY_ASSERT(shader != nullptr);
    parentRefs.emplace_back(this, entry.Target);
    return render::ShaderEntry{
        .Target = shader,
        .EntryPoint = entry.EntryPoint,
    };
}

GpuGraphicsPipelineStateHandle GpuResourceRegistry::CreateGraphicsPipelineState(const GpuGraphicsPipelineStateDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto* rootSig = this->FindAliveRootSignature(desc.RootSig);
    RADRAY_ASSERT(rootSig != nullptr);

    vector<ParentRef> parentRefs{};
    parentRefs.reserve(3);
    parentRefs.emplace_back(this, desc.RootSig);

    render::GraphicsPipelineStateDescriptor nativeDesc{};
    nativeDesc.RootSig = rootSig;
    if (desc.VS.has_value()) {
        nativeDesc.VS = this->MakeNativeShaderEntry(*desc.VS, parentRefs);
    }
    if (desc.PS.has_value()) {
        nativeDesc.PS = this->MakeNativeShaderEntry(*desc.PS, parentRefs);
    }
    nativeDesc.VertexLayouts = desc.VertexLayouts;
    nativeDesc.Primitive = desc.Primitive;
    nativeDesc.DepthStencil = desc.DepthStencil;
    nativeDesc.MultiSample = desc.MultiSample;
    nativeDesc.ColorTargets = desc.ColorTargets;

    auto psoOpt = _device->CreateGraphicsPipelineState(nativeDesc);
    if (!psoOpt.HasValue()) {
        throw GpuSystemException("Device::CreateGraphicsPipelineState failed");
    }

    auto pso = psoOpt.Release();
    auto* native = pso.get();
    return this->AddRecord<GpuGraphicsPipelineStateHandle>(
        GraphicsPipelineStateRecord{std::move(pso)},
        native,
        std::move(parentRefs));
}

GpuComputePipelineStateHandle GpuResourceRegistry::CreateComputePipelineState(const GpuComputePipelineStateDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto* rootSig = this->FindAliveRootSignature(desc.RootSig);
    RADRAY_ASSERT(rootSig != nullptr);

    vector<ParentRef> parentRefs{};
    parentRefs.reserve(2);
    parentRefs.emplace_back(this, desc.RootSig);

    render::ComputePipelineStateDescriptor nativeDesc{};
    nativeDesc.RootSig = rootSig;
    nativeDesc.CS = this->MakeNativeShaderEntry(desc.CS, parentRefs);

    auto psoOpt = _device->CreateComputePipelineState(nativeDesc);
    if (!psoOpt.HasValue()) {
        throw GpuSystemException("Device::CreateComputePipelineState failed");
    }

    auto pso = psoOpt.Release();
    auto* native = pso.get();
    return this->AddRecord<GpuComputePipelineStateHandle>(
        ComputePipelineStateRecord{std::move(pso)},
        native,
        std::move(parentRefs));
}

GpuAccelerationStructureHandle GpuResourceRegistry::CreateAccelerationStructure(const render::AccelerationStructureDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto asOpt = _device->CreateAccelerationStructure(desc);
    if (!asOpt.HasValue()) {
        throw GpuSystemException("Device::CreateAccelerationStructure failed");
    }

    auto as = asOpt.Release();
    auto* native = as.get();
    return this->AddRecord<GpuAccelerationStructureHandle>(
        AccelerationStructureRecord{std::move(as)},
        native);
}

GpuAccelerationStructureViewHandle GpuResourceRegistry::CreateAccelerationStructureView(const GpuAccelerationStructureViewDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto* as = this->FindAliveAccelerationStructure(desc.Target);
    RADRAY_ASSERT(as != nullptr);

    render::AccelerationStructureViewDescriptor nativeDesc{};
    nativeDesc.Target = as;
    auto viewOpt = _device->CreateAccelerationStructureView(nativeDesc);
    if (!viewOpt.HasValue()) {
        throw GpuSystemException("Device::CreateAccelerationStructureView failed");
    }

    auto view = viewOpt.Release();
    auto* native = view.get();
    return this->AddRecord<GpuAccelerationStructureViewHandle>(
        AccelerationStructureViewRecord{std::move(view)},
        native,
        vector<ParentRef>{{this, desc.Target}});
}

render::RayTracingShaderEntry GpuResourceRegistry::MakeNativeRayTracingShaderEntry(
    const GpuRayTracingShaderEntry& entry,
    vector<ParentRef>& parentRefs) {
    auto* shader = this->FindAliveShader(entry.Target);
    RADRAY_ASSERT(shader != nullptr);
    parentRefs.emplace_back(this, entry.Target);
    return render::RayTracingShaderEntry{
        .Target = shader,
        .EntryPoint = entry.EntryPoint,
        .Stage = entry.Stage,
    };
}

GpuRayTracingPipelineStateHandle GpuResourceRegistry::CreateRayTracingPipelineState(const GpuRayTracingPipelineStateDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto* rootSig = this->FindAliveRootSignature(desc.RootSig);
    RADRAY_ASSERT(rootSig != nullptr);

    vector<ParentRef> parentRefs{};
    parentRefs.reserve(1 + desc.ShaderEntries.size() + desc.HitGroups.size() * 3);
    parentRefs.emplace_back(this, desc.RootSig);

    vector<render::RayTracingShaderEntry> shaderEntries{};
    shaderEntries.reserve(desc.ShaderEntries.size());
    for (const GpuRayTracingShaderEntry& entry : desc.ShaderEntries) {
        shaderEntries.emplace_back(this->MakeNativeRayTracingShaderEntry(entry, parentRefs));
    }

    vector<render::RayTracingHitGroupDescriptor> hitGroups{};
    hitGroups.reserve(desc.HitGroups.size());
    for (const GpuRayTracingHitGroupDescriptor& hitGroupDesc : desc.HitGroups) {
        render::RayTracingHitGroupDescriptor nativeHitGroup{};
        nativeHitGroup.Name = hitGroupDesc.Name;
        if (hitGroupDesc.ClosestHit.has_value()) {
            nativeHitGroup.ClosestHit = this->MakeNativeRayTracingShaderEntry(*hitGroupDesc.ClosestHit, parentRefs);
        }
        if (hitGroupDesc.AnyHit.has_value()) {
            nativeHitGroup.AnyHit = this->MakeNativeRayTracingShaderEntry(*hitGroupDesc.AnyHit, parentRefs);
        }
        if (hitGroupDesc.Intersection.has_value()) {
            nativeHitGroup.Intersection = this->MakeNativeRayTracingShaderEntry(*hitGroupDesc.Intersection, parentRefs);
        }
        hitGroups.emplace_back(nativeHitGroup);
    }

    render::RayTracingPipelineStateDescriptor nativeDesc{};
    nativeDesc.RootSig = rootSig;
    nativeDesc.ShaderEntries = std::span<const render::RayTracingShaderEntry>{shaderEntries.data(), shaderEntries.size()};
    nativeDesc.HitGroups = std::span<const render::RayTracingHitGroupDescriptor>{hitGroups.data(), hitGroups.size()};
    nativeDesc.MaxRecursionDepth = desc.MaxRecursionDepth;
    nativeDesc.MaxPayloadSize = desc.MaxPayloadSize;
    nativeDesc.MaxAttributeSize = desc.MaxAttributeSize;
    auto psoOpt = _device->CreateRayTracingPipelineState(nativeDesc);
    if (!psoOpt.HasValue()) {
        throw GpuSystemException("Device::CreateRayTracingPipelineState failed");
    }

    auto pso = psoOpt.Release();
    auto* native = pso.get();
    return this->AddRecord<GpuRayTracingPipelineStateHandle>(
        RayTracingPipelineStateRecord{std::move(pso)},
        native,
        std::move(parentRefs));
}

GpuShaderBindingTableHandle GpuResourceRegistry::CreateShaderBindingTable(const GpuShaderBindingTableDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto* pipeline = this->FindAliveRayTracingPipelineState(desc.Pipeline);
    RADRAY_ASSERT(pipeline != nullptr);

    render::ShaderBindingTableDescriptor nativeDesc{};
    nativeDesc.Pipeline = pipeline;
    nativeDesc.RayGenCount = desc.RayGenCount;
    nativeDesc.MissCount = desc.MissCount;
    nativeDesc.HitGroupCount = desc.HitGroupCount;
    nativeDesc.CallableCount = desc.CallableCount;
    nativeDesc.MaxLocalDataSize = desc.MaxLocalDataSize;
    auto sbtOpt = _device->CreateShaderBindingTable(nativeDesc);
    if (!sbtOpt.HasValue()) {
        throw GpuSystemException("Device::CreateShaderBindingTable failed");
    }

    auto sbt = sbtOpt.Release();
    auto* native = sbt.get();
    return this->AddRecord<GpuShaderBindingTableHandle>(
        ShaderBindingTableRecord{std::move(sbt)},
        native,
        vector<ParentRef>{{this, desc.Pipeline}});
}

GpuBindlessArrayHandle GpuResourceRegistry::CreateBindlessArray(const render::BindlessArrayDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto bindlessArrayOpt = _device->CreateBindlessArray(desc);
    if (!bindlessArrayOpt.HasValue()) {
        throw GpuSystemException("Device::CreateBindlessArray failed");
    }

    auto bindlessArray = bindlessArrayOpt.Release();
    auto* native = bindlessArray.get();
    return this->AddRecord<GpuBindlessArrayHandle>(
        BindlessArrayRecord{std::move(bindlessArray)},
        native);
}

render::Texture* GpuResourceRegistry::FindAliveTexture(const GpuTextureHandle& handle) noexcept {
    return this->FindAliveAs<TextureRecord, render::Texture>(handle);
}

const render::Texture* GpuResourceRegistry::FindAliveTexture(const GpuTextureHandle& handle) const noexcept {
    return this->FindAliveAs<TextureRecord, render::Texture>(handle);
}

render::Shader* GpuResourceRegistry::FindAliveShader(const GpuShaderHandle& handle) noexcept {
    return this->FindAliveAs<ShaderRecord, render::Shader>(handle);
}

render::RootSignature* GpuResourceRegistry::FindAliveRootSignature(const GpuRootSignatureHandle& handle) noexcept {
    return this->FindAliveAs<RootSignatureRecord, render::RootSignature>(handle);
}

render::AccelerationStructure* GpuResourceRegistry::FindAliveAccelerationStructure(const GpuAccelerationStructureHandle& handle) noexcept {
    return this->FindAliveAs<AccelerationStructureRecord, render::AccelerationStructure>(handle);
}

render::RayTracingPipelineState* GpuResourceRegistry::FindAliveRayTracingPipelineState(const GpuRayTracingPipelineStateHandle& handle) noexcept {
    return this->FindAliveAs<RayTracingPipelineStateRecord, render::RayTracingPipelineState>(handle);
}

bool GpuResourceRegistry::Contains(const GpuResourceHandle& handle) const noexcept {
    return this->FindRecord(handle) != nullptr;
}

bool GpuResourceRegistry::IsPendingDestroy(const GpuResourceHandle& handle) const noexcept {
    const auto* record = this->FindRecord(handle);
    return record != nullptr && record->State == State::PendingDestroy;
}

GpuResourceRegistry::Kind GpuResourceRegistry::GetKind(const GpuResourceHandle& handle) const noexcept {
    const auto* record = this->FindRecord(handle);
    return record != nullptr ? record->GetKind() : Kind::Unknown;
}

void GpuResourceRegistry::MarkPendingDestroy(const GpuResourceHandle& handle) {
    auto* record = this->FindRecord(handle);
    RADRAY_ASSERT(record != nullptr);
    record->State = State::PendingDestroy;
}

void GpuResourceRegistry::AddChildRef(const GpuResourceHandle& handle) {
    auto* record = this->FindRecord(handle);
    RADRAY_ASSERT(record != nullptr);
    RADRAY_ASSERT(record->State == State::Alive);
    ++record->ChildRefCount;
}

void GpuResourceRegistry::ReleaseChildRef(const GpuResourceHandle& handle) noexcept {
    auto* record = this->FindRecord(handle);
    if (record == nullptr) {
        return;
    }
    if (record->ChildRefCount > 0) {
        --record->ChildRefCount;
    }
}

bool GpuResourceRegistry::TryRetire(const GpuResourceHandle& handle) noexcept {
    auto* record = this->FindRecord(handle);
    if (record == nullptr) {
        return true;
    }
    if (record->State != State::PendingDestroy) {
        return false;
    }
    if (record->ChildRefCount != 0) {
        return false;
    }

    this->EraseRecord(handle);
    return true;
}

bool GpuResourceRegistry::TryDestroyImmediately(const GpuResourceHandle& handle) noexcept {
    auto* record = this->FindRecord(handle);
    if (record == nullptr) {
        return false;
    }
    if (record->State != State::Alive) {
        return false;
    }
    if (record->ChildRefCount != 0) {
        return false;
    }

    this->EraseRecord(handle);
    return true;
}

void GpuResourceRegistry::Clear() noexcept {
    while (!_records.Empty()) {
        vector<GpuResourceHandle> readyHandles{};
        readyHandles.reserve(_records.Count());
        for (const auto& record : _records.Values()) {
            if (record.ChildRefCount == 0) {
                readyHandles.emplace_back(GpuResourceHandle{
                    record.RecordHandle.Index,
                    record.RecordHandle.Generation,
                    record.NativeHandle});
            }
        }

        if (readyHandles.empty()) {
            RADRAY_ASSERT(false);
            _records.Clear();
            return;
        }

        for (const GpuResourceHandle& handle : readyHandles) {
            this->EraseRecord(handle);
        }
    }
}

const GpuResourceRegistry::ResourceRecord* GpuResourceRegistry::FindRecord(const GpuResourceHandle& handle) const noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const SparseSetHandle recordHandle = ToRecordHandle(handle);
    const auto* record = _records.TryGet(recordHandle);
    if (record == nullptr) {
        return nullptr;
    }
    if (record->NativeHandle != handle.NativeHandle) {
        return nullptr;
    }
    return record;
}

GpuResourceRegistry::ResourceRecord* GpuResourceRegistry::FindRecord(const GpuResourceHandle& handle) noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const SparseSetHandle recordHandle = ToRecordHandle(handle);
    auto* record = _records.TryGet(recordHandle);
    if (record == nullptr) {
        return nullptr;
    }
    if (record->NativeHandle != handle.NativeHandle) {
        return nullptr;
    }
    return record;
}

void GpuResourceRegistry::EraseRecord(const GpuResourceHandle& handle) noexcept {
    const SparseSetHandle recordHandle = ToRecordHandle(handle);
    auto* recordPtr = _records.TryGet(recordHandle);
    if (recordPtr == nullptr || recordPtr->NativeHandle != handle.NativeHandle) {
        return;
    }

    ResourceRecord record = std::move(*recordPtr);
    _records.Destroy(recordHandle);

    for (const ParentRef& parent : record.ParentRefs) {
        if (parent.Registry != nullptr && parent.Handle.IsValid()) {
            parent.Registry->ReleaseChildRef(parent.Handle);
        }
    }
}

render::SwapChainDescriptor GpuSurface::GetDesc() const {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        RADRAY_ASSERT(_swapchain != nullptr);
        return _swapchain->GetDesc();
    }
    RADRAY_ASSERT(_swapchain != nullptr);
    return _swapchain->GetDesc();
}

uint32_t GpuSurface::GetQueueSlot() const {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        return _queueSlot;
    }
    return _queueSlot;
}

size_t GpuSurface::GetNextFrameSlotIndex() const {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        return _nextFrameSlotIndex;
    }
    return _nextFrameSlotIndex;
}

uint32_t GpuSurface::GetWidth() const {
    return this->GetDesc().Width;
}

uint32_t GpuSurface::GetHeight() const {
    return this->GetDesc().Height;
}

render::TextureFormat GpuSurface::GetFormat() const {
    return this->GetDesc().Format;
}

render::PresentMode GpuSurface::GetPresentMode() const {
    return this->GetDesc().PresentMode;
}

uint32_t GpuSurface::GetFlightFrameCount() const {
    return this->GetDesc().FlightFrameCount;
}

GpuAsyncContext::~GpuAsyncContext() noexcept = default;

GpuAsyncContext::GpuAsyncContext(
    GpuRuntime* runtime,
    render::CommandQueue* queue,
    uint32_t queueSlot) noexcept
    : _runtime(runtime),
      _queue(queue),
      _queueSlot(queueSlot),
      _resourceRegistry(runtime != nullptr && runtime->_device != nullptr
                            ? make_unique<GpuResourceRegistry>(runtime->_device.get())
                            : nullptr) {}

bool GpuAsyncContext::IsEmpty() const {
    return _cmdBuffers.empty();
}

bool GpuAsyncContext::DependsOn(const GpuTask& task) {
    if (!task.IsValid()) {
        return false;
    }
    RADRAY_ASSERT(_runtime == task._runtime);
    _waitFences.emplace_back(task._fence);
    _waitValues.emplace_back(task._signalValue);
    return true;
}

render::CommandBuffer* GpuAsyncContext::CreateCommandBuffer() {
    auto cmdBufferOpt = _runtime->_device->CreateCommandBuffer(_queue);
    if (!cmdBufferOpt.HasValue()) {
        throw GpuSystemException("Device::CreateCommandBuffer failed");
    }
    auto& cmdBuffer = _cmdBuffers.emplace_back(cmdBufferOpt.Release());
    return cmdBuffer.get();
}

GpuBufferHandle GpuAsyncContext::CreateTransientBuffer(const render::BufferDescriptor& desc) {
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateBuffer(desc);
}

GpuTextureHandle GpuAsyncContext::CreateTransientTexture(const render::TextureDescriptor& desc) {
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateTexture(desc);
}

GpuTextureViewHandle GpuAsyncContext::CreateTransientTextureView(const GpuTextureViewDescriptor& desc) {
    RADRAY_ASSERT(_resourceRegistry != nullptr);

    GpuResourceRegistry::ParentResourceRef parent{};
    if (auto* localTexture = _resourceRegistry->FindAliveTexture(desc.Target); localTexture != nullptr) {
        parent.Registry = _resourceRegistry.get();
        parent.Handle = desc.Target;
        parent.NativeHandle = localTexture;
        return _resourceRegistry->CreateTextureView(desc, parent);
    }

    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        if (_runtime->_resourceRegistry != nullptr) {
            auto* persistentTexture = _runtime->_resourceRegistry->FindAliveTexture(desc.Target);
            if (persistentTexture != nullptr) {
                parent.Registry = _runtime->_resourceRegistry.get();
                parent.Handle = desc.Target;
                parent.NativeHandle = persistentTexture;
                return _resourceRegistry->CreateTextureView(desc, parent);
            }
        }
    }

    RADRAY_ASSERT(parent.Registry != nullptr);
    RADRAY_ASSERT(parent.Handle.IsValid());
    RADRAY_ASSERT(parent.NativeHandle != nullptr);
    return _resourceRegistry->CreateTextureView(desc, parent);
}

GpuFrameContext::GpuFrameContext(
    GpuRuntime* runtime,
    GpuSurface* surface,
    size_t frameSlotIndex,
    render::Texture* backBuffer,
    uint32_t backBufferIndex) noexcept
    : GpuAsyncContext(runtime, surface->_swapchain->GetDesc().PresentQueue, surface->_queueSlot),
      _surface(surface),
      _frameSlotIndex(frameSlotIndex),
      _backBuffer(backBuffer),
      _backBufferIndex(backBufferIndex) {}

GpuFrameContext::~GpuFrameContext() noexcept {
    RADRAY_ASSERT(_consumeState != ConsumeState::Acquired);
}

render::Texture* GpuFrameContext::GetBackBuffer() const {
    return _backBuffer;
}

uint32_t GpuFrameContext::GetBackBufferIndex() const {
    return _backBufferIndex;
}

GpuRuntime::GpuRuntime(
    shared_ptr<render::Device> device,
    unique_ptr<render::InstanceVulkan> vkInstance) noexcept
    : _device(std::move(device)),
      _vkInstance(std::move(vkInstance)),
      _resourceRegistry(_device != nullptr ? make_unique<GpuResourceRegistry>(_device.get()) : nullptr) {}

GpuRuntime::~GpuRuntime() noexcept {
    this->Destroy();
}

bool GpuRuntime::IsValid() const {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    return _device != nullptr;
}

render::Device* GpuRuntime::GetDevice() const {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    return _device.get();
}

void GpuRuntime::Destroy() noexcept {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    _pendings.clear();
    _resourceRetirements.clear();
    _resourceRegistry.reset();
    for (auto& fences : _queueFences) {
        fences.clear();
    }
    _device.reset();
    if (_vkInstance != nullptr) {
        render::DestroyVulkanInstance(std::move(_vkInstance));
    }
}

unique_ptr<GpuSurface> GpuRuntime::CreateSurface(const GpuSurfaceDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(desc.FlightFrameCount > 0);
    RADRAY_ASSERT(_device != nullptr);
    auto queueOpt = _device->GetCommandQueue(render::QueueType::Direct, desc.QueueSlot);
    if (!queueOpt.HasValue()) {
        throw GpuSystemException("Device::GetCommandQueue failed");
    }
    auto queue = queueOpt.Get();
    render::SwapChainDescriptor swapChainDesc{};
    swapChainDesc.PresentQueue = queue;
    swapChainDesc.NativeHandler = desc.NativeHandler;
    swapChainDesc.Width = desc.Width;
    swapChainDesc.Height = desc.Height;
    swapChainDesc.BackBufferCount = desc.BackBufferCount;
    swapChainDesc.Format = desc.Format;
    swapChainDesc.PresentMode = desc.PresentMode;
    swapChainDesc.FlightFrameCount = desc.FlightFrameCount;
    auto swapchainOpt = _device->CreateSwapChain(swapChainDesc);
    if (!swapchainOpt.HasValue()) {
        throw GpuSystemException("Device::CreateSwapChain failed");
    }
    auto result = make_unique<GpuSurface>(this, swapchainOpt.Release(), desc.QueueSlot);
    const uint32_t actualFlightFrameCount = result->_swapchain->GetDesc().FlightFrameCount;
    result->_frameSlots.reserve(actualFlightFrameCount);
    for (uint32_t i = 0; i < actualFlightFrameCount; i++) {
        result->_frameSlots.emplace_back();
    }
    return result;
}

GpuBufferHandle GpuRuntime::CreateBuffer(const render::BufferDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateBuffer(desc);
}

GpuTextureHandle GpuRuntime::CreateTexture(const render::TextureDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateTexture(desc);
}

GpuTextureViewHandle GpuRuntime::CreateTextureView(const GpuTextureViewDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);

    auto* texture = _resourceRegistry->FindAliveTexture(desc.Target);
    RADRAY_ASSERT(texture != nullptr);

    GpuResourceRegistry::ParentResourceRef parent{};
    parent.Registry = _resourceRegistry.get();
    parent.Handle = desc.Target;
    parent.NativeHandle = texture;
    return _resourceRegistry->CreateTextureView(desc, parent);
}

GpuSamplerHandle GpuRuntime::CreateSampler(const render::SamplerDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateSampler(desc);
}

GpuShaderHandle GpuRuntime::CreateShader(const render::ShaderDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateShader(desc);
}

GpuRootSignatureHandle GpuRuntime::CreateRootSignature(const GpuRootSignatureDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateRootSignature(desc);
}

GpuDescriptorSetHandle GpuRuntime::CreateDescriptorSet(const GpuDescriptorSetDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateDescriptorSet(desc);
}

GpuDescriptorSetHandle GpuRuntime::CreateDescriptorSet(GpuRootSignatureHandle rootSig, render::DescriptorSetIndex set) {
    GpuDescriptorSetDescriptor desc{};
    desc.RootSig = rootSig;
    desc.Set = set;
    return this->CreateDescriptorSet(desc);
}

GpuGraphicsPipelineStateHandle GpuRuntime::CreateGraphicsPipelineState(const GpuGraphicsPipelineStateDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateGraphicsPipelineState(desc);
}

GpuComputePipelineStateHandle GpuRuntime::CreateComputePipelineState(const GpuComputePipelineStateDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateComputePipelineState(desc);
}

GpuAccelerationStructureHandle GpuRuntime::CreateAccelerationStructure(const render::AccelerationStructureDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateAccelerationStructure(desc);
}

GpuAccelerationStructureViewHandle GpuRuntime::CreateAccelerationStructureView(const GpuAccelerationStructureViewDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateAccelerationStructureView(desc);
}

GpuRayTracingPipelineStateHandle GpuRuntime::CreateRayTracingPipelineState(const GpuRayTracingPipelineStateDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateRayTracingPipelineState(desc);
}

GpuShaderBindingTableHandle GpuRuntime::CreateShaderBindingTable(const GpuShaderBindingTableDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateShaderBindingTable(desc);
}

GpuBindlessArrayHandle GpuRuntime::CreateBindlessArray(const render::BindlessArrayDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateBindlessArray(desc);
}

void GpuRuntime::DestroyResourceImmediate(GpuResourceHandle handle) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    RADRAY_ASSERT(_resourceRegistry->Contains(handle));
    RADRAY_ASSERT(!_resourceRegistry->IsPendingDestroy(handle));
    const bool destroyed = _resourceRegistry->TryDestroyImmediately(handle);
    RADRAY_ASSERT(destroyed);
}

void GpuRuntime::DestroyResourceAfter(GpuResourceHandle handle, const GpuTask& task) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    RADRAY_ASSERT(_resourceRegistry->Contains(handle));
    RADRAY_ASSERT(!_resourceRegistry->IsPendingDestroy(handle));
    RADRAY_ASSERT(task.IsValid());
    RADRAY_ASSERT(task._runtime == this);

    _resourceRegistry->MarkPendingDestroy(handle);
    ResourceRetirement retirement{};
    retirement.Handle = handle;
    retirement.Fence = task._fence;
    retirement.SignalValue = task._signalValue;
    _resourceRetirements.emplace_back(std::move(retirement));
}

GpuRuntime::BeginFrameResult GpuRuntime::BeginFrame(GpuSurface* surface) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(surface != nullptr);
    RADRAY_ASSERT(surface->_swapchain != nullptr);
    RADRAY_ASSERT(surface->_runtime == this);
    auto* fence = this->GetQueueFenceUnlocked(render::QueueType::Direct, surface->_queueSlot);
    const size_t frameSlotIndex = surface->_nextFrameSlotIndex;
    auto& nowFrame = surface->_frameSlots[frameSlotIndex];
    fence->Wait(nowFrame._fenceValue);
    return this->AcquireSwapChainUnlocked(surface, std::numeric_limits<uint64_t>::max());
}

GpuRuntime::BeginFrameResult GpuRuntime::TryBeginFrame(GpuSurface* surface) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(surface != nullptr);
    RADRAY_ASSERT(surface->_swapchain != nullptr);
    RADRAY_ASSERT(surface->_runtime == this);
    auto* fence = this->GetQueueFenceUnlocked(render::QueueType::Direct, surface->_queueSlot);
    const size_t frameSlotIndex = surface->_nextFrameSlotIndex;
    auto& nowFrame = surface->_frameSlots[frameSlotIndex];
    if (fence->GetCompletedValue() < nowFrame._fenceValue) {
        return {nullptr, render::SwapChainStatus::RetryLater};
    }
    return this->AcquireSwapChainUnlocked(surface, 0);
}

unique_ptr<GpuAsyncContext> GpuRuntime::BeginAsync(render::QueueType type, uint32_t queueSlot) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_device != nullptr);
    auto queueOpt = _device->GetCommandQueue(type, queueSlot);
    if (!queueOpt.HasValue()) {
        throw GpuSystemException("Device::GetCommandQueue failed");
    }
    auto queue = queueOpt.Get();
    return make_unique<GpuAsyncContext>(this, queue, queueSlot);
}

GpuRuntime::SubmittedBatch GpuRuntime::SubmitContextUnlocked(
    GpuAsyncContext* context,
    Nullable<render::SwapChainSyncObject*> waitToExecute,
    Nullable<render::SwapChainSyncObject*> readyToPresent) {
    RADRAY_ASSERT(context != nullptr);
    RADRAY_ASSERT(!context->IsEmpty());

    vector<render::CommandBuffer*> submitCmds;
    submitCmds.reserve(context->_cmdBuffers.size());
    for (const auto& cmdBuffer : context->_cmdBuffers) {
        submitCmds.emplace_back(cmdBuffer.get());
    }

    auto* fence = this->GetQueueFenceUnlocked(context->_queue->GetQueueType(), context->_queueSlot);
    const uint64_t signalValue = fence->GetLastSignaledValue() + 1;
    render::Fence* signalFences[] = {fence};
    uint64_t signalValues[] = {signalValue};

    render::CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = submitCmds;
    submitDesc.SignalFences = signalFences;
    submitDesc.SignalValues = signalValues;
    submitDesc.WaitFences = context->_waitFences;
    submitDesc.WaitValues = context->_waitValues;

    render::SwapChainSyncObject* swapChainWait[] = {nullptr};
    render::SwapChainSyncObject* swapChainPresent[] = {nullptr};
    if (waitToExecute != nullptr) {
        swapChainWait[0] = waitToExecute.Get();
        submitDesc.WaitToExecute = swapChainWait;
    }
    if (readyToPresent != nullptr) {
        swapChainPresent[0] = readyToPresent.Get();
        submitDesc.ReadyToPresent = swapChainPresent;
    }

    context->_queue->Submit(submitDesc);
    return SubmittedBatch{fence, signalValue};
}

void GpuRuntime::ValidateFrameContextForConsume(const GpuFrameContext* context) const {
    RADRAY_UNUSED(context);
    RADRAY_ASSERT(context != nullptr);
    RADRAY_ASSERT(context->GetType() == GpuAsyncContext::Kind::Frame);
    RADRAY_ASSERT(context->_runtime == this);
    RADRAY_ASSERT(context->_surface != nullptr);
    RADRAY_ASSERT(context->_surface->_swapchain != nullptr);
    RADRAY_ASSERT(context->_surface->_runtime == this);
}

GpuRuntime::SubmitFrameResult GpuRuntime::FinalizeFrameUnlocked(unique_ptr<GpuFrameContext> context) {
    this->ValidateFrameContextForConsume(context.get());
    auto* surface = context->_surface;
    const size_t frameSlotIndex = context->_frameSlotIndex;
    const auto submitted = this->SubmitContextUnlocked(context.get(), context->_waitToDraw, context->_readyToPresent);
    GpuTask task{this, submitted.Fence, submitted.SignalValue};
    RADRAY_ASSERT(context->_acquiredFrame.has_value());
    const auto present = surface->_swapchain->Present(std::move(context->_acquiredFrame.value()));
    context->_acquiredFrame.reset();
    surface->_frameSlots[frameSlotIndex]._fenceValue = submitted.SignalValue;
    _pendings.emplace_back(std::move(context), submitted.Fence, submitted.SignalValue);
    return {std::move(task), present};
}

GpuTask GpuRuntime::SubmitAsync(unique_ptr<GpuAsyncContext> context) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(context != nullptr);
    RADRAY_ASSERT(context->GetType() == GpuAsyncContext::Kind::Async);
    RADRAY_ASSERT(context->_runtime == this);
    const auto submitted = this->SubmitContextUnlocked(context.get(), nullptr, nullptr);
    _pendings.emplace_back(std::move(context), submitted.Fence, submitted.SignalValue);
    return GpuTask{this, submitted.Fence, submitted.SignalValue};
}

GpuRuntime::SubmitFrameResult GpuRuntime::SubmitFrame(unique_ptr<GpuFrameContext> context) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    this->ValidateFrameContextForConsume(context.get());
    context->_consumeState = GpuFrameContext::ConsumeState::Submitted;
    return this->FinalizeFrameUnlocked(std::move(context));
}

GpuRuntime::SubmitFrameResult GpuRuntime::AbandonFrame(unique_ptr<GpuFrameContext> context) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    this->ValidateFrameContextForConsume(context.get());
    context->_consumeState = GpuFrameContext::ConsumeState::Abandoned;
    context->_cmdBuffers.clear();
    context->_waitFences.clear();
    context->_waitValues.clear();

    auto* cmd = context->CreateCommandBuffer();
    cmd->Begin();
    if (context->_backBuffer != nullptr) {
        render::ResourceBarrierDescriptor toPresent = render::BarrierTextureDescriptor{
            .Target = context->_backBuffer,
            .Before = render::TextureState::Undefined,
            .After = render::TextureState::Present,
        };
        cmd->ResourceBarrier(std::span{&toPresent, 1});
    }
    cmd->End();

    return this->FinalizeFrameUnlocked(std::move(context));
}

void GpuRuntime::ProcessTasks() {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    this->ProcessTasksUnlocked();
}

void GpuRuntime::ProcessTasksUnlocked() {
    std::erase_if(_pendings, [](const Pending& pending) {
        return pending._fence != nullptr && pending._fence->GetCompletedValue() >= pending._signalValue;
    });

    if (_resourceRegistry == nullptr) {
        _resourceRetirements.clear();
    } else {
        bool madeProgress = false;
        do {
            madeProgress = false;
            std::erase_if(_resourceRetirements, [this, &madeProgress](const ResourceRetirement& retirement) {
                if (!retirement.IsReady()) {
                    return false;
                }
                const auto kind = _resourceRegistry->GetKind(retirement.Handle);
                if (kind == GpuResourceRegistry::Kind::Unknown) {
                    madeProgress = true;
                    return true;
                }
                if (_resourceRegistry->TryRetire(retirement.Handle)) {
                    madeProgress = true;
                    return true;
                }
                return false;
            });
        } while (madeProgress);
    }
}

void GpuRuntime::Wait(render::QueueType type, uint32_t queueSlot) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_device != nullptr);
    auto queueOpt = _device->GetCommandQueue(type, queueSlot);
    if (!queueOpt.HasValue()) {
        throw GpuSystemException("Device::GetCommandQueue failed");
    }
    auto queue = queueOpt.Get();
    queue->Wait();
    this->ProcessTasksUnlocked();
}

render::Fence* GpuRuntime::GetQueueFenceUnlocked(render::QueueType type, uint32_t slot) {
    RADRAY_ASSERT(_device != nullptr);
#ifdef RADRAY_ENABLE_D3D12
    if (_device->GetBackend() == render::RenderBackend::D3D12) {
        auto deviceD3D12 = render::d3d12::CastD3D12Object(_device.get());
        auto queueD3D12Opt = deviceD3D12->GetCommandQueue(type, slot);
        if (!queueD3D12Opt.HasValue()) {
            throw GpuSystemException("Device::GetCommandQueue failed");
        }
        auto queueD3D12 = render::d3d12::CastD3D12Object(queueD3D12Opt.Get());
        return queueD3D12->_fence.get();
    }
#endif
    auto& fences = _queueFences[(size_t)type];
    if (fences.size() <= slot) {
        fences.reserve(slot + 1);
        for (size_t i = fences.size(); i <= slot; i++) {
            fences.emplace_back(nullptr);
        }
    }
    auto& fence = fences[slot];
    if (!fence) {
        auto fenceOpt = _device->CreateFence();
        if (!fenceOpt.HasValue()) {
            throw GpuSystemException("Device::CreateFence failed");
        }
        fence = fenceOpt.Release();
    }
    return fence.get();
}

GpuRuntime::BeginFrameResult GpuRuntime::AcquireSwapChainUnlocked(GpuSurface* surface, uint64_t timeoutMs) {
    const size_t frameSlotIndex = surface->_nextFrameSlotIndex;
    auto acqResult = surface->_swapchain->AcquireNext(timeoutMs);
    switch (acqResult.Status) {
        case render::SwapChainStatus::Success: {
            auto& frame = acqResult.Frame.value();
            auto context = make_unique<GpuFrameContext>(
                this,
                surface,
                frameSlotIndex,
                frame.GetBackBuffer(),
                frame.GetBackBufferIndex());
            context->_type = GpuAsyncContext::Kind::Frame;
            context->_waitToDraw = frame.GetWaitToDraw();
            context->_readyToPresent = frame.GetReadyToPresent();
            context->_acquiredFrame = std::move(frame);
            surface->_nextFrameSlotIndex = (frameSlotIndex + 1) % surface->_frameSlots.size();
            return {std::move(context), render::SwapChainStatus::Success};
        }
        case render::SwapChainStatus::RetryLater:
        case render::SwapChainStatus::RequireRecreate:
            return {nullptr, acqResult.Status};
        case render::SwapChainStatus::Error:
        default: {
            throw GpuSystemException("SwapChain::AcquireNext failed");
        }
    }
}

bool GpuRuntime::ResourceRetirement::IsReady() const noexcept {
    return Fence != nullptr && SignalValue != 0 && Fence->GetCompletedValue() >= SignalValue;
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(const render::VulkanDeviceDescriptor& desc, render::VulkanInstanceDescriptor vkInsDesc) {
    auto insOpt = render::CreateVulkanInstance(vkInsDesc);
    if (!insOpt.HasValue()) {
        return nullptr;
    }
    auto deviceOpt = render::CreateDevice(desc);
    if (!deviceOpt.HasValue()) {
        return nullptr;
    }
    return make_unique<GpuRuntime>(deviceOpt.Release(), insOpt.Release());
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(const render::D3D12DeviceDescriptor& desc) {
    auto deviceOpt = render::CreateDevice(desc);
    if (!deviceOpt.HasValue()) {
        return nullptr;
    }
    return make_unique<GpuRuntime>(deviceOpt.Release(), nullptr);
}

}  // namespace radray
