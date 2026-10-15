#include "rhi_validation.h"
#include "gpu_test_fixture.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <radray/render/pipeline_layout_types.h>
#include <radray/render/backend_shader_artifact.h>
#include <radray/file.h>

namespace radray::render::test {
namespace {

struct ValidationProbe {
    std::mutex LogMutex;
    vector<string> Errors;
    std::atomic<uint32_t> BufferCreates{0};
    uint32_t Begins{0}, Ends{0}, Resets{0}, Copies{0}, Barriers{0}, Flushes{0};
    uint32_t PassBegins{0}, VertexBinds{0}, PipelineBinds{0}, QueryResets{0}, Timestamps{0}, QueryResolves{0};
    uint32_t Sets{0}, Binds{0}, Pushes{0}, Dispatches{0}, EndPasses{0}, Submits{0}, Presents{0};
    uint32_t ParameterResets{0};
    uint32_t Recreates{0};
    bool FailSet{false}, FailPass{false}, FailPresent{false}, ConsumePresentError{false};
    bool FailRecreate{false};
    Nullable<CommandQueue*> AllocatorQueue{nullptr};
    Nullable<CommandAllocator*> CommandAllocator{nullptr};
    Nullable<Buffer*> CopySource{nullptr}, CopyDestination{nullptr};
    Nullable<PipelineLayout*> SetLayout{nullptr}, PipelineLayout{nullptr};
    Nullable<Shader*> PipelineShader{nullptr};
    Nullable<ShaderParameterSet*> BoundSet{nullptr};
    BindingHandle WrittenHandle{}, OffsetHandle{};
    std::optional<ShaderParameterValue> WrittenValue;
    vector<ResourceBarrierDescriptor> NativeBarriers;
    vector<MappedBufferRange> NativeFlushes;
    vector<CommandBuffer*> NativeCommands;
    vector<Fence*> NativeFences;
    vector<SwapChainSyncObject*> NativeSyncs, NativePresentSyncs;
    vector<Fence*> NativeWaitFences;
    vector<RenderPassColorAttachment> NativeAttachments;
    vector<VertexBufferBinding> NativeVertexBuffers;
    Nullable<QueryPool*> NativeQuery{nullptr};
    Nullable<Buffer*> NativeQueryDestination{nullptr};
    Nullable<CommandQueue*> CalibrationQueue{nullptr};

    static void Log(LogLevel, std::string_view message, void* data) {
        auto& probe = *static_cast<ValidationProbe*>(data);
        std::lock_guard lock{probe.LogMutex};
        probe.Errors.emplace_back(message);
    }
};

template <class T>
class ProbeObject : public T {
public:
    bool IsValid() const noexcept override { return Live; }
    void Destroy() noexcept override { Live = false; }
    bool Live{true};
};

template <class T>
class NamedProbeObject : public ProbeObject<T> {
public:
    void SetDebugName(std::string_view) noexcept override {}
};

class ProbeBuffer final : public NamedProbeObject<Buffer> {
public:
    ProbeBuffer(Device* owner, BufferDescriptor desc) : Owner(owner), Desc(desc), Bytes(static_cast<size_t>(desc.Size)) {}
    void* Map(uint64_t offset, uint64_t) noexcept override { return Bytes.data() + offset; }
    void Unmap() noexcept override {}
    void FlushMappedRange(BufferRange) noexcept override {}
    void InvalidateMappedRange(BufferRange) noexcept override {}
    BufferDescriptor GetDesc() const noexcept override { return Desc; }
    Device* GetDevice() const noexcept override { return Owner; }
    Device* Owner;
    BufferDescriptor Desc;
    vector<byte> Bytes;
};

class ProbeTexture final : public NamedProbeObject<Texture> {
public:
    ProbeTexture(Device* owner, TextureDescriptor desc) : Owner(owner), Desc(desc) {}
    TextureDescriptor GetDesc() const noexcept override { return Desc; }
    Device* GetDevice() const noexcept override { return Owner; }
    Device* Owner;
    TextureDescriptor Desc;
};

class ProbeView final : public NamedProbeObject<TextureView> {
public:
    explicit ProbeView(TextureViewDescriptor desc) : Desc(desc) {}
    TextureViewDescriptor GetDesc() const noexcept override { return Desc; }
    TextureViewDescriptor Desc;
};

class ProbeFence final : public NamedProbeObject<Fence> {
public:
    uint64_t GetCompletedValue() const noexcept override { return Signaled; }
    uint64_t GetLastSignaledValue() const noexcept override { return Signaled; }
    void Wait() noexcept override {}
    void Wait(uint64_t) noexcept override {}
    bool WaitFor(uint64_t value, uint64_t) noexcept override { return Signaled >= value; }
    uint64_t Signaled{0};
};

class ProbeQueue final : public ProbeObject<CommandQueue> {
public:
    explicit ProbeQueue(ValidationProbe& probe) : Probe(probe) {}
    void Submit(const CommandQueueSubmitDescriptor& desc) noexcept override {
        ++Probe.Submits;
        Probe.NativeCommands.assign(desc.CmdBuffers.begin(), desc.CmdBuffers.end());
        Probe.NativeFences.assign(desc.SignalFences.begin(), desc.SignalFences.end());
        Probe.NativeSyncs.assign(desc.WaitToExecute.begin(), desc.WaitToExecute.end());
        Probe.NativePresentSyncs.assign(desc.ReadyToPresent.begin(), desc.ReadyToPresent.end());
        Probe.NativeWaitFences.assign(desc.WaitFences.begin(), desc.WaitFences.end());
        for (size_t i = 0; i < desc.SignalFences.size(); ++i) static_cast<ProbeFence*>(desc.SignalFences[i])->Signaled = desc.SignalValues[i];
    }
    void Wait() noexcept override {}
    QueueType GetQueueType() const noexcept override { return QueueType::Direct; }
    TimestampClockCalibrationResult GetTimestampClockCalibration() const noexcept override { return {}; }
    ValidationProbe& Probe;
};

class ProbeQuery final : public NamedProbeObject<QueryPool> {
public:
    ProbeQuery(ValidationProbe& probe, QueryPoolDescriptor desc) : Probe(probe), Desc(desc) {}
    QueryType GetType() const noexcept override { return Desc.Type; }
    uint32_t GetCount() const noexcept override { return Desc.Count; }
    TimestampQueryCalibration GetTimestampCalibration(CommandQueue* queue) const noexcept override {
        Probe.CalibrationQueue = queue;
        return {1000000, 1000};
    }
    ValidationProbe& Probe;
    QueryPoolDescriptor Desc;
};

class ProbeAllocator final : public NamedProbeObject<CommandAllocator> {
public:
    explicit ProbeAllocator(ValidationProbe& probe) : Probe(probe) {}
    void Reset() noexcept override { ++Probe.Resets; }
    ValidationProbe& Probe;
};

template <class T>
class ProbeEncoder : public ProbeObject<T> {
public:
    ProbeEncoder(ValidationProbe& probe, CommandBuffer* command) : Probe(probe), Command(command) {}
    CommandBuffer* GetCommandBuffer() const noexcept override { return Command; }
    void BindShaderParameterSet(uint32_t, ShaderParameterSet* set, std::span<const ShaderParameterDynamicOffset> offsets) noexcept override {
        ++Probe.Binds;
        Probe.BoundSet = set;
        if (!offsets.empty()) Probe.OffsetHandle = offsets.front().Binding;
    }
    bool SetPushConstants(BindingHandle, std::span<const byte>) noexcept override {
        ++Probe.Pushes;
        return true;
    }
    ValidationProbe& Probe;
    CommandBuffer* Command;
};

class ProbeComputeEncoder final : public ProbeEncoder<ComputeCommandEncoder> {
public:
    using ProbeEncoder::ProbeEncoder;
    void BindComputePipelineState(ComputePipelineState*) noexcept override {}
    void Dispatch(uint32_t, uint32_t, uint32_t) noexcept override { ++Probe.Dispatches; }
    void DispatchIndirect(Buffer*, uint64_t) noexcept override { ++Probe.Dispatches; }
};

class ProbeGraphicsEncoder final : public ProbeEncoder<GraphicsCommandEncoder> {
public:
    using ProbeEncoder::ProbeEncoder;
    void SetViewport(Viewport) noexcept override {}
    void SetScissor(Rect) noexcept override {}
    void BindVertexBuffers(std::span<const VertexBufferBinding> bindings) noexcept override {
        ++Probe.VertexBinds;
        Probe.NativeVertexBuffers.assign(bindings.begin(), bindings.end());
    }
    void BindIndexBuffer(IndexBufferView) noexcept override {}
    void BindGraphicsPipelineState(GraphicsPipelineState*) noexcept override { ++Probe.PipelineBinds; }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) noexcept override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) noexcept override {}
    void DrawIndirect(Buffer*, uint64_t, uint32_t) noexcept override {}
    void DrawIndexedIndirect(Buffer*, uint64_t, uint32_t) noexcept override {}
};

class ProbeCommand final : public NamedProbeObject<CommandBuffer> {
public:
    explicit ProbeCommand(ValidationProbe& probe) : Probe(probe) {}
    void Begin() noexcept override { ++Probe.Begins; }
    void End() noexcept override { ++Probe.Ends; }
    void PushDebugGroup(std::string_view) noexcept override {}
    void PopDebugGroup() noexcept override {}
    void ResourceBarrier(std::span<const ResourceBarrierDescriptor> barriers) noexcept override {
        ++Probe.Barriers;
        Probe.NativeBarriers.assign(barriers.begin(), barriers.end());
    }
    Nullable<unique_ptr<GraphicsCommandEncoder>> BeginRenderPass(const RenderPassBeginDescriptor& desc) noexcept override {
        ++Probe.PassBegins;
        Probe.NativeAttachments.assign(desc.Colors.begin(), desc.Colors.end());
        if (Probe.FailPass) return nullptr;
        return make_unique<ProbeGraphicsEncoder>(Probe, this);
    }
    void EndRenderPass(unique_ptr<GraphicsCommandEncoder>&& encoder) noexcept override {
        ++Probe.EndPasses;
        encoder.reset();
    }
    Nullable<unique_ptr<ComputeCommandEncoder>> BeginComputePass() noexcept override { return make_unique<ProbeComputeEncoder>(Probe, this); }
    void EndComputePass(unique_ptr<ComputeCommandEncoder>&& encoder) noexcept override {
        ++Probe.EndPasses;
        encoder.reset();
    }
    void CopyBufferToBuffer(Buffer* destination, uint64_t, Buffer* source, uint64_t, uint64_t) noexcept override {
        ++Probe.Copies;
        Probe.CopyDestination = destination;
        Probe.CopySource = source;
    }
    void CopyBufferToTexture(Texture*, SubresourceRange, Buffer*, uint64_t) noexcept override { ++Probe.Copies; }
    bool CopyBufferToTextureRegion(const BufferToTextureCopyDescriptor&) noexcept override {
        ++Probe.Copies;
        return true;
    }
    void CopyTextureToBuffer(Buffer*, uint64_t, Texture*, SubresourceRange) noexcept override { ++Probe.Copies; }
    void CopyTextureToTexture(const TextureCopyDescriptor&) noexcept override { ++Probe.Copies; }
    void ResolveTexture(const TextureResolveDescriptor&) noexcept override { ++Probe.Copies; }
    void ResetQueryPool(QueryPool* pool, uint32_t, uint32_t) noexcept override {
        ++Probe.QueryResets;
        Probe.NativeQuery = pool;
    }
    void WriteTimestamp(const QueryTimestampDescriptor& desc) noexcept override {
        ++Probe.Timestamps;
        Probe.NativeQuery = desc.Pool;
    }
    void ResolveQueryData(const QueryResolveDescriptor& desc) noexcept override {
        ++Probe.QueryResolves;
        Probe.NativeQuery = desc.Pool;
        Probe.NativeQueryDestination = desc.Destination;
    }
    ValidationProbe& Probe;
};

class ProbeLayout final : public NamedProbeObject<PipelineLayout> {
public:
    BindingHandle FindBinding(std::string_view name) const noexcept override {
        for (uint32_t i = 0; i < Names.size(); ++i)
            if (Names[i] == name) return BindingHandleAccess::Make(i, reinterpret_cast<uintptr_t>(this));
        return {};
    }
    vector<string> Names;
};

class ProbeSet final : public ProbeObject<ShaderParameterSet> {
public:
    explicit ProbeSet(ValidationProbe& probe) : Probe(probe) {}
    bool Set(BindingHandle handle, uint32_t, ShaderParameterValue value) noexcept override {
        ++Probe.Sets;
        Probe.WrittenHandle = handle;
        Probe.WrittenValue = value;
        return !Probe.FailSet;
    }
    bool FlushWrites() noexcept override { return true; }
    void ResetBindingsForReuse() noexcept override { ++Probe.ParameterResets; }
    ValidationProbe& Probe;
};

class ProbeShader final : public ProbeObject<Shader> {
public:
    explicit ProbeShader(ShaderStages stages) : Stages(stages) {}
    ShaderStages GetStages() const noexcept override { return Stages; }
    ShaderStages Stages;
};

class ProbeSwapchain final : public ProbeObject<SwapChain> {
public:
    ProbeSwapchain(ValidationProbe& probe, Device* device, SwapChainDescriptor desc) : Probe(probe), Desc(desc), Image(device, {.Dim = TextureDimension::Dim2D, .Width = desc.Width, .Height = desc.Height, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = desc.Format, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}) {}
    SwapChainAcquireResult AcquireNext(uint64_t) noexcept override {
        if (!Live) return {.Status = SwapChainStatus::RequireRecreate};
        return {.Status = SwapChainStatus::Success, .Frame = MakeFrame(this, ++Token, &Image, 0, &AcquireSync, &PresentSync)};
    }
    SwapChainPresentResult Present(SwapChainFrame&& frame) noexcept override {
        ++Probe.Presents;
        if (!Probe.FailPresent || Probe.ConsumePresentError) InvalidateFrame(frame);
        return {.Status = Probe.FailPresent ? SwapChainStatus::Error : SwapChainStatus::Success};
    }
    SwapChainPresentResult DiscardAcquiredFrame(SwapChainFrame&& frame) noexcept override { return Present(std::move(frame)); }
    bool Recreate(uint32_t width, uint32_t height, TextureFormat, PresentMode) noexcept override {
        ++Probe.Recreates;
        Live = !Probe.FailRecreate;
        if (!Live) return false;
        Desc.Width = width;
        Desc.Height = height;
        Image.Desc.Width = width;
        Image.Desc.Height = height;
        return true;
    }
    uint32_t GetBackBufferCount() const noexcept override { return Desc.BackBufferCount; }
    SwapChainDescriptor GetDesc() const noexcept override { return Desc; }
    ValidationProbe& Probe;
    SwapChainDescriptor Desc;
    ProbeTexture Image;
    ProbeObject<SwapChainSyncObject> AcquireSync, PresentSync;
    uint64_t Token{0};
};

class ProbeDevice final : public ProbeObject<Device>, public detail::D3D12PipelineLayoutFactory {
public:
    explicit ProbeDevice(ValidationProbe& probe) : Probe(probe), Queue(probe) {
        Capabilities.Detail.MaxVertexInputBindings = 16;
        Capabilities.Limits = {.MaxColorAttachments = 8, .MaxTexture1DDimension = 16384, .MaxTexture2DDimension = 16384, .MaxTexture3DDimension = 2048, .MaxTextureArrayLayers = 2048, .MaxBufferSize = UINT64_MAX, .MaxUniformBufferRange = 65536, .MaxPushConstantBytes = 128, .CBufferOffsetAlignment = 256, .StorageBufferOffsetAlignment = 16};
        Capabilities.Features = {.TimestampQueries = true, .IndirectDraw = true, .IndirectDispatch = true, .SubresourceBarriers = true, .UavMemoryBarrier = true, .UavWriteStages = ShaderStage::Graphics | ShaderStage::Compute};
        Capabilities.Queues[0].CreatedCount = 1;
    }
    RenderBackend GetBackend() noexcept override { return Backend; }
    DeviceDetail GetDetail() const noexcept override { return Capabilities.Detail; }
    const RenderDeviceCapabilities& GetCapabilities() const noexcept override { return Capabilities; }
    TextureSupport QueryTextureSupport(const TextureSupportQuery&) const noexcept override { return {true, SampleCount::X1 | SampleCount::X4, true, true, 16384, 16384, 2048, 2048, 15, UINT64_MAX}; }
    Nullable<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot) noexcept override { return type == QueueType::Direct && slot == 0 ? &Queue : nullptr; }
    Nullable<unique_ptr<CommandAllocator>> CreateCommandAllocator(CommandQueue* queue) noexcept override {
        Probe.AllocatorQueue = queue;
        return make_unique<ProbeAllocator>(Probe);
    }
    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(CommandAllocator* allocator) noexcept override {
        Probe.CommandAllocator = allocator;
        return make_unique<ProbeCommand>(Probe);
    }
    Nullable<unique_ptr<Fence>> CreateFence() noexcept override { return make_unique<ProbeFence>(); }
    Nullable<unique_ptr<QueryPool>> CreateQueryPool(const QueryPoolDescriptor& desc) noexcept override { return make_unique<ProbeQuery>(Probe, desc); }
    Nullable<unique_ptr<SwapChain>> CreateSwapChain(const SwapChainDescriptor& desc) noexcept override { return make_unique<ProbeSwapchain>(Probe, this, desc); }
    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc) noexcept override {
        ++Probe.BufferCreates;
        return make_unique<ProbeBuffer>(this, desc);
    }
    void FlushMappedRanges(std::span<const MappedBufferRange> ranges) noexcept override {
        ++Probe.Flushes;
        Probe.NativeFlushes.assign(ranges.begin(), ranges.end());
    }
    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc) noexcept override { return make_unique<ProbeTexture>(this, desc); }
    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc) noexcept override { return make_unique<ProbeView>(desc); }
    Nullable<unique_ptr<Shader>> CreateShader(const ShaderDescriptor& desc) noexcept override { return make_unique<ProbeShader>(desc.Stages); }
    Nullable<unique_ptr<PipelineLayout>> CreatePipelineLayout(const ResolvedD3D12Layout& desc) noexcept override {
        auto layout = make_unique<ProbeLayout>();
        for (const auto& binding : desc.Bindings) layout->Names.push_back(binding.Name);
        for (const auto& push : desc.PushConstants) layout->Names.push_back(push.Name);
        return layout;
    }
    Nullable<unique_ptr<ShaderParameterSet>> CreateShaderParameterSet(const ShaderParameterSetDescriptor& desc) noexcept override {
        Probe.SetLayout = desc.Layout;
        return make_unique<ProbeSet>(Probe);
    }
    Nullable<unique_ptr<GraphicsPipelineState>> CreateGraphicsPipelineState(const GraphicsPipelineStateDescriptor& desc) noexcept override {
        Probe.PipelineLayout = desc.PipelineLayout;
        Probe.PipelineShader = desc.VS->Target;
        return make_unique<NamedProbeObject<GraphicsPipelineState>>();
    }
    Nullable<unique_ptr<ComputePipelineState>> CreateComputePipelineState(const ComputePipelineStateDescriptor& desc) noexcept override {
        Probe.PipelineLayout = desc.PipelineLayout;
        Probe.PipelineShader = desc.CS.Target;
        return make_unique<NamedProbeObject<ComputePipelineState>>();
    }
    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor&) noexcept override { return make_unique<NamedProbeObject<Sampler>>(); }
    Nullable<Sampler*> GetOrCreateSampler(const SamplerDescriptor&) noexcept override { return &CachedSampler; }
    ValidationProbe& Probe;
    RenderDeviceCapabilities Capabilities;
    RenderBackend Backend{RenderBackend::D3D12};
    ProbeQueue Queue;
    NamedProbeObject<render::Sampler> CachedSampler;
};

class RhiValidationTest : public testing::Test {
protected:
    ValidationProbe Probe;
    shared_ptr<ProbeDevice> Native = make_shared<ProbeDevice>(Probe);
    shared_ptr<Device> Device = detail::CreateValidationDevice(Native, {.IsEnabled = true, .LogCallback = ValidationProbe::Log, .LogUserData = &Probe}).Unwrap();
    CommandQueue* Queue = Device->GetCommandQueue(QueueType::Direct).Get();
    unique_ptr<Buffer> Buffer(BufferUses usage = BufferUse::CopySource | BufferUse::CopyDestination | BufferUse::MapWrite | BufferUse::CBuffer | BufferUse::Resource | BufferUse::UnorderedAccess) {
        return Device->CreateBuffer({1024, MemoryType::Upload, usage}).Unwrap();
    }
    unique_ptr<PipelineLayout> Layout() {
        ResolvedD3D12Layout desc;
        desc.Bindings = {{"Buffer", shader::ShaderBindingKind::CBuffer, 0, 0, 1, ShaderStage::Compute, shader::ShaderBindingPlacement::RootDescriptor}};
        desc.PushConstants = {{"Push", 0, 1, 0, 16, ShaderStage::Compute}};
        return dynamic_cast<detail::D3D12PipelineLayoutFactory*>(Device.get())->CreatePipelineLayout(desc).Unwrap();
    }
    unique_ptr<Shader> Shader() {
        const array<byte, 4> source{};
        return Device->CreateShader({source, ShaderBlobCategory::DXIL, ShaderStage::Compute}).Unwrap();
    }
};

TEST(RhiValidationOptionsTest, DefaultsFollowBuildConfiguration) {
#if defined(RADRAY_IS_DEBUG) && !defined(NDEBUG)
    EXPECT_TRUE(RhiValidationOptions{}.IsEnabled);
#else
    EXPECT_FALSE(RhiValidationOptions{}.IsEnabled);
#endif
    EXPECT_FALSE((RhiValidationOptions{.IsEnabled = false}).IsEnabled);
    EXPECT_TRUE((RhiValidationOptions{.IsEnabled = true}).IsEnabled);
}

TEST_F(RhiValidationTest, StableBorrowedIdentityAndNestedReferencesAlwaysUseWrappers) {
    EXPECT_EQ(Device->GetCommandQueue(QueueType::Direct).Get(), Queue);
    EXPECT_NE(Queue, &Native->Queue);
    auto* sampler = Device->GetOrCreateSampler({}).Get();
    EXPECT_EQ(Device->GetOrCreateSampler({}).Get(), sampler);
    EXPECT_NE(sampler, &Native->CachedSampler);
    auto buffer = Buffer();
    EXPECT_EQ(buffer->GetDevice(), Device.get());
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    EXPECT_EQ(Probe.AllocatorQueue.Get(), &Native->Queue);
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    EXPECT_NE(Probe.CommandAllocator.Get(), allocator.get());
    command->Begin();
    auto encoder = command->BeginComputePass().Unwrap();
    EXPECT_EQ(encoder->GetCommandBuffer(), command.get());
    command->EndComputePass(std::move(encoder));
    EXPECT_FALSE(encoder);
    command->End();
    EXPECT_TRUE(Probe.Errors.empty());
}

TEST_F(RhiValidationTest, BatchesPreflightEveryItemBeforeForwardingAndRejectCrossDevice) {
    ValidationProbe otherProbe;
    auto otherNative = make_shared<ProbeDevice>(otherProbe);
    auto otherDevice = detail::CreateValidationDevice(otherNative, {.IsEnabled = true}).Unwrap();
    auto foreign = otherDevice->CreateBuffer({1024, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite}).Unwrap();
    auto local = Buffer();
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor barriers[]{BarrierBufferDescriptor{local.get(), BufferState::CopySource, BufferState::CopyDestination}, BarrierBufferDescriptor{foreign.get(), BufferState::CopySource, BufferState::CopyDestination}};
    command->ResourceBarrier(barriers);
    EXPECT_EQ(Probe.Barriers, 0u);
    const MappedBufferRange ranges[]{{local.get(), {0, 256}}, {foreign.get(), {0, 256}}};
    Device->FlushMappedRanges(ranges);
    EXPECT_EQ(Probe.Flushes, 0u);
    command->CopyBufferToBuffer(local.get(), 0, foreign.get(), 0, 16);
    EXPECT_EQ(Probe.Copies, 0u);
    command->CopyBufferToBuffer(local.get(), 0, local.get(), 32, 16);
    EXPECT_EQ(Probe.Copies, 1u);
    EXPECT_NE(Probe.CopySource.Get(), local.get());
    EXPECT_EQ(Probe.CopySource->GetDevice(), Native.get());
    command->ResourceBarrier(std::span{barriers, 1});
    EXPECT_EQ(Probe.Barriers, 1u);
    EXPECT_NE(std::get<BarrierBufferDescriptor>(Probe.NativeBarriers[0]).Target, local.get());
    Device->FlushMappedRanges(std::span{ranges, 1});
    EXPECT_EQ(Probe.Flushes, 1u);
    EXPECT_NE(Probe.NativeFlushes[0].Target, local.get());
    command->End();
    EXPECT_GE(Probe.Errors.size(), 3u);
}

TEST_F(RhiValidationTest, InvalidRecordingAndEncoderPairingLeaveOwnershipAndStateRetryable) {
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto secondAllocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto first = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    auto second = Device->CreateCommandBuffer(secondAllocator.get()).Unwrap();
    first->End();
    EXPECT_EQ(Probe.Ends, 0u);
    first->Begin();
    first->Begin();
    EXPECT_EQ(Probe.Begins, 1u);
    allocator->Reset();
    EXPECT_EQ(Probe.Resets, 0u);
    auto encoder = first->BeginComputePass().Unwrap();
    second->Begin();
    second->EndComputePass(std::move(encoder));
    EXPECT_TRUE(encoder);
    EXPECT_EQ(Probe.EndPasses, 0u);
    first->End();
    EXPECT_EQ(Probe.Ends, 0u);
    first->EndComputePass(std::move(encoder));
    EXPECT_FALSE(encoder);
    EXPECT_EQ(Probe.EndPasses, 1u);
    first->End();
    second->End();
    EXPECT_EQ(Probe.Ends, 2u);
    first->Begin();
    EXPECT_EQ(Probe.Begins, 2u);
    allocator->Reset();
    first->Begin();
    first->End();
    EXPECT_EQ(Probe.Begins, 3u);
    secondAllocator->Reset();
    CommandBuffer* commands[]{first.get(), second.get()};
    Queue->Submit({.CmdBuffers = commands});
    EXPECT_EQ(Probe.Submits, 0u);
    Queue->Submit({.CmdBuffers = std::span{commands, 1}});
    EXPECT_EQ(Probe.Submits, 1u);
    EXPECT_NE(Probe.NativeCommands[0], first.get());
    Queue->Submit({.CmdBuffers = std::span{commands, 1}});
    EXPECT_EQ(Probe.Submits, 1u);
}

TEST_F(RhiValidationTest, D3D12AllocatorRejectsOverlapUntilRecordingSuccessfullyEnds) {
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto first = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    auto second = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    first->Begin();
    second->Begin();
    EXPECT_EQ(Probe.Begins, 1u);
    ASSERT_EQ(Probe.Errors.size(), 1u);
    auto encoder = first->BeginComputePass().Unwrap();
    first->End();
    second->Begin();
    EXPECT_EQ(Probe.Ends, 0u);
    EXPECT_EQ(Probe.Begins, 1u);
    first->EndComputePass(std::move(encoder));
    first->PushDebugGroup("Still open");
    first->End();
    second->Begin();
    EXPECT_EQ(Probe.Ends, 0u);
    EXPECT_EQ(Probe.Begins, 1u);
    first->PopDebugGroup();
    first->End();
    second->Begin();
    EXPECT_EQ(Probe.Ends, 1u);
    EXPECT_EQ(Probe.Begins, 2u);
    second->End();
    allocator->Reset();
    first->Begin();
    first->End();
    EXPECT_EQ(Probe.Begins, 3u);
    EXPECT_EQ(Probe.Resets, 1u);
    EXPECT_EQ(Probe.Errors.size(), 5u);
}

TEST_F(RhiValidationTest, VulkanAllocatorAllowsInterleavedRecordings) {
    Native->Backend = RenderBackend::Vulkan;
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto first = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    auto second = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    first->Begin();
    second->Begin();
    EXPECT_EQ(Probe.Begins, 2u);
    second->End();
    first->End();
    allocator->Reset();
    EXPECT_EQ(Probe.Resets, 1u);
    EXPECT_TRUE(Probe.Errors.empty());
}

TEST_F(RhiValidationTest, HandleTranslationSealingFullPushSizeAndNativeFailureCommitOnlyOnSuccess) {
    auto layout = Layout(), foreignLayout = Layout();
    auto buffer = Buffer();
    auto set = Device->CreateShaderParameterSet({layout.get(), 0}).Unwrap();
    EXPECT_NE(Probe.SetLayout.Get(), layout.get());
    const auto binding = layout->FindBinding("Buffer"), push = layout->FindBinding("Push");
    EXPECT_FALSE(set->Set(foreignLayout->FindBinding("Buffer"), 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    EXPECT_FALSE(set->Set(push, 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    EXPECT_FALSE(set->Set(binding, 1, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    EXPECT_FALSE(set->Set(binding, 0, ShaderBufferBinding{buffer.get(), {1, 256}, 0}));
    EXPECT_EQ(Probe.Sets, 0u);
    auto shader = Shader();
    auto pipeline = Device->CreateComputePipelineState({layout.get(), {shader.get(), "main"}}).Unwrap();
    EXPECT_NE(Probe.PipelineLayout.Get(), layout.get());
    EXPECT_NE(Probe.PipelineShader.Get(), shader.get());
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    auto encoder = command->BeginComputePass().Unwrap();
    encoder->BindComputePipelineState(pipeline.get());
    const ShaderParameterDynamicOffset offset{binding, 256};
    Probe.FailSet = true;
    EXPECT_FALSE(set->Set(binding, 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    encoder->BindShaderParameterSet(0, set.get(), std::span{&offset, 1});
    EXPECT_EQ(Probe.Binds, 0u);
    Probe.FailSet = false;
    EXPECT_TRUE(set->Set(binding, 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    EXPECT_NE(Probe.WrittenHandle, binding);
    EXPECT_NE(std::get<ShaderBufferBinding>(*Probe.WrittenValue).Target, buffer.get());
    encoder->BindShaderParameterSet(0, set.get());
    EXPECT_EQ(Probe.Binds, 0u);
    const ShaderParameterDynamicOffset duplicates[]{offset, offset};
    encoder->BindShaderParameterSet(0, set.get(), duplicates);
    EXPECT_EQ(Probe.Binds, 0u);
    EXPECT_TRUE(set->Set(binding, 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    encoder->BindShaderParameterSet(0, set.get(), std::span{&offset, 1});
    EXPECT_EQ(Probe.Binds, 1u);
    EXPECT_EQ(Probe.OffsetHandle, Probe.WrittenHandle);
    EXPECT_NE(Probe.BoundSet.Get(), set.get());
    EXPECT_FALSE(set->Set(binding, 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    set->ResetBindingsForReuse();
    EXPECT_EQ(Probe.ParameterResets, 0u);
    EXPECT_FALSE(set->Set(binding, 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    const array<byte, 16> bytes{};
    EXPECT_FALSE(encoder->SetPushConstants(push, std::span{bytes}.first(8)));
    EXPECT_FALSE(encoder->SetPushConstants(binding, bytes));
    EXPECT_FALSE(encoder->SetPushConstants(foreignLayout->FindBinding("Push"), bytes));
    EXPECT_EQ(Probe.Pushes, 0u);
    EXPECT_TRUE(encoder->SetPushConstants(push, bytes));
    EXPECT_EQ(Probe.Pushes, 1u);
    encoder->Dispatch(1, 1, 1);
    EXPECT_EQ(Probe.Dispatches, 1u);
    command->EndComputePass(std::move(encoder));
    command->End();
    allocator->Reset();
    set->ResetBindingsForReuse();
    EXPECT_EQ(Probe.ParameterResets, 1u);
    EXPECT_TRUE(set->Set(binding, 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
}

TEST_F(RhiValidationTest, SwapchainTokensTrackNativeConsumptionAndKeepBorrowedIdentity) {
    auto chain = Device->CreateSwapChain({.PresentQueue = Queue, .NativeHandler = reinterpret_cast<void*>(1), .Width = 8, .Height = 8, .BackBufferCount = 2, .Format = TextureFormat::RGBA8_UNORM, .PresentMode = PresentMode::FIFO}).Unwrap();
    EXPECT_EQ(chain->GetDesc().PresentQueue, Queue);
    auto acquired = chain->AcquireNext();
    ASSERT_TRUE(acquired.Frame);
    auto& frame = *acquired.Frame;
    auto* image = frame.GetBackBuffer();
    auto* sync = frame.GetWaitToDraw();
    EXPECT_EQ(image->GetDevice(), Device.get());
    EXPECT_TRUE(frame.IsFrom(chain.get()));
    SwapChainSyncObject* waits[]{sync};
    SwapChainSyncObject* presents[]{frame.GetReadyToPresent()};
    Queue->Submit({.WaitToExecute = waits, .ReadyToPresent = presents});
    EXPECT_EQ(Probe.Submits, 1u);
    EXPECT_NE(Probe.NativeSyncs[0], sync);
    EXPECT_NE(Probe.NativePresentSyncs[0], presents[0]);
    SwapChainSyncObject* invalidWaits[]{sync, nullptr};
    Queue->Submit({.WaitToExecute = invalidWaits});
    EXPECT_EQ(Probe.Submits, 1u);
    EXPECT_FALSE(chain->AcquireNext().Frame);
    auto foreignChain = Device->CreateSwapChain(chain->GetDesc()).Unwrap();
    EXPECT_EQ(foreignChain->Present(std::move(frame)).Status, SwapChainStatus::Error);
    EXPECT_TRUE(frame.IsValid());
    EXPECT_EQ(Probe.Presents, 0u);
    auto view = Device->CreateTextureView({image, TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    EXPECT_EQ(view->GetDesc().Target, image);
    Probe.FailPresent = true;
    EXPECT_EQ(chain->Present(std::move(frame)).Status, SwapChainStatus::Error);
    EXPECT_TRUE(frame.IsValid());
    Probe.FailPresent = false;
    EXPECT_EQ(chain->Present(std::move(frame)).Status, SwapChainStatus::Success);
    EXPECT_FALSE(frame.IsValid());
    EXPECT_FALSE(chain->Recreate(16, 16, TextureFormat::RGBA8_UNORM, PresentMode::FIFO));
    view.reset();
    auto next = chain->AcquireNext();
    EXPECT_EQ(next.Frame->GetBackBuffer(), image);
    EXPECT_EQ(next.Frame->GetWaitToDraw(), sync);
    Probe.FailPresent = Probe.ConsumePresentError = true;
    EXPECT_EQ(chain->Present(std::move(*next.Frame)).Status, SwapChainStatus::Error);
    EXPECT_FALSE(next.Frame->IsValid());
    EXPECT_TRUE(chain->Recreate(16, 16, TextureFormat::RGBA8_UNORM, PresentMode::FIFO));
}

TEST_F(RhiValidationTest, NativeSwapchainRecreateFailurePreservesTheRecoveryPath) {
    auto chain = Device->CreateSwapChain({.PresentQueue = Queue, .NativeHandler = reinterpret_cast<void*>(1), .Width = 8, .Height = 8, .BackBufferCount = 2, .Format = TextureFormat::RGBA8_UNORM, .PresentMode = PresentMode::FIFO}).Unwrap();
    auto acquired = chain->AcquireNext();
    ASSERT_TRUE(acquired.Frame);
    EXPECT_EQ(chain->Present(std::move(*acquired.Frame)).Status, SwapChainStatus::Success);
    EXPECT_FALSE(chain->Recreate(0, 8, TextureFormat::RGBA8_UNORM, PresentMode::FIFO));
    EXPECT_EQ(Probe.Recreates, 0u);
    const auto errors = Probe.Errors.size();
    Probe.FailRecreate = true;
    EXPECT_FALSE(chain->Recreate(16, 16, TextureFormat::RGBA8_UNORM, PresentMode::FIFO));
    EXPECT_FALSE(chain->IsValid());
    EXPECT_EQ(chain->GetDesc().PresentQueue, Queue);
    EXPECT_EQ(chain->AcquireNext().Status, SwapChainStatus::RequireRecreate);
    Probe.FailRecreate = false;
    EXPECT_TRUE(chain->Recreate(16, 16, TextureFormat::RGBA8_UNORM, PresentMode::FIFO));
    EXPECT_TRUE(chain->IsValid());
    acquired = chain->AcquireNext();
    ASSERT_TRUE(acquired.Frame);
    EXPECT_EQ(acquired.Frame->GetBackBuffer()->GetDevice(), Device.get());
    EXPECT_EQ(acquired.Frame->GetBackBuffer()->GetDesc().Width, 16u);
    EXPECT_EQ(chain->Present(std::move(*acquired.Frame)).Status, SwapChainStatus::Success);
    EXPECT_EQ(Probe.Recreates, 2u);
    EXPECT_EQ(Probe.Errors.size(), errors);
}

TEST_F(RhiValidationTest, RenderAttachmentsPipelinesAndVertexBatchesValidateBeforeNativeEffects) {
    auto texture = Device->CreateTexture({TextureDimension::Dim2D, 8, 8, 1, 1, 1, TextureFormat::RGBA8_UNORM, MemoryType::Device, TextureUse::RenderTarget}).Unwrap();
    auto view = Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    auto layout = dynamic_cast<detail::D3D12PipelineLayoutFactory*>(Device.get())->CreatePipelineLayout({}).Unwrap();
    const array<byte, 4> bytes{};
    auto shader = Device->CreateShader({bytes, ShaderBlobCategory::DXIL, ShaderStage::Vertex}).Unwrap();
    const array vertexLayouts{VertexBufferLayout{0, 12, VertexStepMode::Vertex}};
    const array colorTargets{ColorTargetState::Default(TextureFormat::RGBA8_UNORM)};
    auto pipelineDesc = GraphicsPipelineStateDescriptor{.PipelineLayout = layout.get(), .VS = ShaderEntry{shader.get(), "main"}, .VertexInput = {vertexLayouts, {}}, .Primitive = PrimitiveState::Default(), .MultiSample = MultiSampleState::Default(), .ColorTargets = colorTargets};
    auto pipeline = Device->CreateGraphicsPipelineState(pipelineDesc).Unwrap();
    const array otherTargets{ColorTargetState::Default(TextureFormat::BGRA8_UNORM)};
    pipelineDesc.ColorTargets = otherTargets;
    auto incompatible = Device->CreateGraphicsPipelineState(pipelineDesc).Unwrap();
    auto vertex = Buffer(BufferUse::Vertex | BufferUse::MapWrite);
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    array colors{RenderPassColorAttachment{view.get(), LoadAction::Clear, StoreAction::Store, {}}};
    auto begin = RenderPassBeginDescriptor{.Colors = colors, .Width = 8, .Height = 8};
    auto invalid = begin;
    invalid.Width = 9;
    EXPECT_FALSE(command->BeginRenderPass(invalid));
    EXPECT_EQ(Probe.PassBegins, 0u);
    Probe.FailPass = true;
    EXPECT_FALSE(command->BeginRenderPass(begin));
    EXPECT_EQ(Probe.PassBegins, 1u);
    // Native failure must not pin the attachments or start an encoder.
    view->Destroy();
    EXPECT_FALSE(view->IsValid());
    view = Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    colors[0].View = view.get();
    Probe.FailPass = false;
    auto encoder = command->BeginRenderPass(begin).Unwrap();
    EXPECT_EQ(Probe.PassBegins, 2u);
    EXPECT_NE(Probe.NativeAttachments[0].View, view.get());
    EXPECT_NE(Probe.NativeAttachments[0].View->GetDesc().Target, texture.get());
    encoder->BindGraphicsPipelineState(incompatible.get());
    EXPECT_EQ(Probe.PipelineBinds, 0u);
    encoder->BindGraphicsPipelineState(pipeline.get());
    EXPECT_EQ(Probe.PipelineBinds, 1u);
    const VertexBufferBinding badBindings[]{{0, {vertex.get(), 0, 48}}, {1, {nullptr, 0, 48}}};
    encoder->BindVertexBuffers(badBindings);
    EXPECT_EQ(Probe.VertexBinds, 0u);
    encoder->BindVertexBuffers(std::span{badBindings, 1});
    EXPECT_EQ(Probe.VertexBinds, 1u);
    EXPECT_NE(Probe.NativeVertexBuffers[0].View.Target, vertex.get());
    command->EndRenderPass(std::move(encoder));
    EXPECT_FALSE(encoder);
    command->End();
    view->Destroy();
    EXPECT_TRUE(view->IsValid());
    allocator->Reset();
    view->Destroy();
    EXPECT_FALSE(view->IsValid());
}

TEST_F(RhiValidationTest, ReadOnlyAttachmentsRejectWritingPipelinesWithoutNativeBindings) {
    auto texture = Device->CreateTexture({TextureDimension::Dim2D, 8, 8, 1, 1, 1, TextureFormat::D24_UNORM_S8_UINT, MemoryType::Device, TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite}).Unwrap();
    auto view = Device->CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::D24_UNORM_S8_UINT, {0, 1, 0, 1}, TextureViewUsage::DepthRead}).Unwrap();
    auto layout = dynamic_cast<detail::D3D12PipelineLayoutFactory*>(Device.get())->CreatePipelineLayout({}).Unwrap();
    const array<byte, 4> bytes{};
    auto shader = Device->CreateShader({bytes, ShaderBlobCategory::DXIL, ShaderStage::Vertex}).Unwrap();
    auto depth = DepthStencilState::Default();
    depth.Format = TextureFormat::D24_UNORM_S8_UINT;
    depth.Stencil = StencilState::Default();
    GraphicsPipelineStateDescriptor descriptor{.PipelineLayout = layout.get(), .VS = ShaderEntry{shader.get(), "main"}, .Primitive = PrimitiveState::Default(), .DepthStencil = depth, .MultiSample = MultiSampleState::Default()};
    auto depthWriter = Device->CreateGraphicsPipelineState(descriptor).Unwrap();
    descriptor.DepthStencil->DepthWriteEnable = false;
    descriptor.DepthStencil->Stencil->Front.PassOp = StencilOperation::Replace;
    auto stencilWriter = Device->CreateGraphicsPipelineState(descriptor).Unwrap();
    descriptor.DepthStencil->Stencil->Front.PassOp = StencilOperation::Keep;
    auto reader = Device->CreateGraphicsPipelineState(descriptor).Unwrap();
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    auto encoder = command->BeginRenderPass({.DepthStencil = RenderPassDepthStencilAttachment{view.get(), LoadAction::Load, StoreAction::Store, LoadAction::Load, StoreAction::Store, true}, .Width = 8, .Height = 8}).Unwrap();
    encoder->BindGraphicsPipelineState(depthWriter.get());
    encoder->BindGraphicsPipelineState(stencilWriter.get());
    EXPECT_EQ(Probe.PipelineBinds, 0u);
    EXPECT_EQ(Probe.Errors.size(), 2u);
    encoder->BindGraphicsPipelineState(reader.get());
    EXPECT_EQ(Probe.PipelineBinds, 1u);
    command->EndRenderPass(std::move(encoder));
    command->End();
}

TEST_F(RhiValidationTest, QueriesAndFenceBatchesUnwrapWithoutCommittingRejectedSubmissions) {
    auto pool = Device->CreateQueryPool({QueryType::Timestamp, 4}).Unwrap();
    EXPECT_EQ(pool->GetCount(), 4u);
    EXPECT_EQ(pool->GetTimestampCalibration(Queue).FrequencyHz, 1000000u);
    EXPECT_EQ(Probe.CalibrationQueue.Get(), &Native->Queue);
    auto destination = Buffer();
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    command->ResetQueryPool(pool.get(), 3, 2);
    EXPECT_EQ(Probe.QueryResets, 0u);
    command->ResetQueryPool(pool.get(), 0, 4);
    EXPECT_EQ(Probe.QueryResets, 1u);
    EXPECT_NE(Probe.NativeQuery.Get(), pool.get());
    command->WriteTimestamp({pool.get(), QueryPipelineStage::Bottom, 4});
    EXPECT_EQ(Probe.Timestamps, 0u);
    command->WriteTimestamp({pool.get(), QueryPipelineStage::Bottom, 0});
    EXPECT_EQ(Probe.Timestamps, 1u);
    command->ResolveQueryData({pool.get(), 0, 4, destination.get(), 1});
    EXPECT_EQ(Probe.QueryResolves, 0u);
    command->ResolveQueryData({pool.get(), 0, 4, destination.get(), 0});
    EXPECT_EQ(Probe.QueryResolves, 1u);
    EXPECT_NE(Probe.NativeQueryDestination.Get(), destination.get());
    command->End();
    auto fence = Device->CreateFence().Unwrap();
    CommandBuffer* commands[]{command.get()};
    Fence* signals[]{fence.get(), nullptr};
    uint64_t values[]{1, 2};
    Queue->Submit({.CmdBuffers = commands, .SignalFences = signals, .SignalValues = values});
    EXPECT_EQ(Probe.Submits, 0u);
    EXPECT_EQ(fence->GetLastSignaledValue(), 0u);
    Queue->Submit({.CmdBuffers = commands, .SignalFences = std::span{signals, 1}, .SignalValues = std::span{values, 1}});
    EXPECT_EQ(Probe.Submits, 1u);
    EXPECT_EQ(fence->GetLastSignaledValue(), 1u);
    EXPECT_NE(Probe.NativeFences[0], fence.get());
    Queue->Submit({.SignalFences = std::span{signals, 1}, .SignalValues = std::span{values, 1}});
    EXPECT_EQ(Probe.Submits, 1u);
    Fence* waits[]{fence.get()};
    uint64_t waitValues[]{1};
    Queue->Submit({.WaitFences = waits, .WaitValues = waitValues});
    EXPECT_EQ(Probe.Submits, 2u);
    EXPECT_NE(Probe.NativeWaitFences[0], fence.get());
    EXPECT_FALSE(Probe.Errors.empty());
}

TEST_F(RhiValidationTest, RegistrySupportsIndependentWorkers) {
    vector<std::jthread> workers;
    for (uint32_t i = 0; i < 8; ++i) workers.emplace_back([this] {
        for (uint32_t item = 0; item < 64; ++item) {
            auto buffer = Buffer();
            buffer->Destroy();
        }
    });
    workers.clear();
    EXPECT_EQ(Probe.BufferCreates.load(), 512u);
    EXPECT_TRUE(Probe.Errors.empty());
}

using RhiValidationDeathTest = RhiValidationTest;
TEST_F(RhiValidationDeathTest, IrrevocableDestructionAndInfallibleQueriesAbort) {
    auto allocator = Device->CreateCommandAllocator(Queue).Unwrap();
    auto command = Device->CreateCommandBuffer(allocator.get()).Unwrap();
    EXPECT_DEATH(allocator.reset(), "");
    command->Begin();
    EXPECT_DEATH(command.reset(), "");
    command->End();
    auto fence = Device->CreateFence().Unwrap();
    fence->Destroy();
    EXPECT_DEATH(fence->GetCompletedValue(), "");
    auto pool = Device->CreateQueryPool({QueryType::Timestamp, 1}).Unwrap();
    EXPECT_DEATH(pool->GetTimestampCalibration(nullptr), "");
    pool->Destroy();
    EXPECT_DEATH(pool->GetTimestampCalibration(Queue), "");
}

class RhiValidationGpuTest : public testing::TestWithParam<std::tuple<RenderBackend, bool>> {};
TEST_P(RhiValidationGpuTest, PublicCopyPathWithValidationOnAndOff) {
    DeviceContext context;
    const auto [backend, validation] = GetParam();
    if (!TryCreateDevice(backend, context, true, validation)) GTEST_SKIP() << context.Reason;
#ifdef RADRAY_ENABLE_D3D12
    if (backend == RenderBackend::D3D12) EXPECT_EQ(dynamic_cast<d3d12::DeviceD3D12*>(context.Device.get()) == nullptr, validation);
#endif
#ifdef RADRAY_ENABLE_VULKAN
    if (backend == RenderBackend::Vulkan) EXPECT_EQ(dynamic_cast<vulkan::DeviceVulkan*>(context.Device.get()) == nullptr, validation);
#endif
    auto source = context.Device->CreateBuffer({256, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite}).Unwrap();
    auto destination = context.Device->CreateBuffer({256, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    auto allocator = context.Device->CreateCommandAllocator(context.Queue).Unwrap();
    auto command = context.Device->CreateCommandBuffer(allocator.get()).Unwrap();
    auto fence = context.Device->CreateFence().Unwrap();
    for (uint64_t frame = 1; frame <= 3; ++frame) {
        auto* bytes = static_cast<uint32_t*>(source->Map(0, 256));
        ASSERT_NE(bytes, nullptr);
        bytes[0] = static_cast<uint32_t>(frame * 71);
        source->FlushMappedRange({0, 256});
        source->Unmap();
        command->Begin();
        if (backend == RenderBackend::Vulkan) {
            const ResourceBarrierDescriptor before[]{BarrierBufferDescriptor{source.get(), BufferState::HostWrite, BufferState::CopySource}, BarrierBufferDescriptor{destination.get(), BufferState::Undefined, BufferState::CopyDestination}};
            command->ResourceBarrier(before);
        }
        command->CopyBufferToBuffer(destination.get(), 0, source.get(), 0, 256);
        if (backend == RenderBackend::Vulkan) {
            const ResourceBarrierDescriptor after = BarrierBufferDescriptor{destination.get(), BufferState::CopyDestination, BufferState::HostRead};
            command->ResourceBarrier(std::span{&after, 1});
        }
        command->End();
        CommandBuffer* commands[]{command.get()};
        Fence* fences[]{fence.get()};
        context.Queue->Submit({.CmdBuffers = commands, .SignalFences = fences, .SignalValues = std::span{&frame, 1}});
        ASSERT_TRUE(fence->WaitFor(frame, 10000));
        destination->InvalidateMappedRange({0, 256});
        auto* result = static_cast<uint32_t*>(destination->Map(0, 256));
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result[0], frame * 71);
        destination->Unmap();
        allocator->Reset();
    }
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}
TEST_P(RhiValidationGpuTest, PublicShaderSourceAcceptsByteAlignedArtifactData) {
    DeviceContext context;
    const auto [backend, validation] = GetParam();
    if (!TryCreateDevice(backend, context, true, validation)) GTEST_SKIP() << context.Reason;
    const auto target = GetShaderTargetForBackend(backend);
    const auto path = std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/shader_artifacts" /
                      (backend == RenderBackend::D3D12 ? "compute.dxil.bin" : "compute.spirv.bin");
    const auto blob = ReadBinaryFile(path);
    ASSERT_TRUE(blob);
    ASSERT_GE(blob->size(), sizeof(shader::WireMetadataEnvelope));
    shader::WireMetadataEnvelope envelope;
    std::memcpy(&envelope, blob->data(), sizeof(envelope));
    const auto artifact = shader::DecodeShaderArtifact(*blob, {.Target = *target, .ExpectedGpuArtifact = envelope.GpuArtifact});
    ASSERT_TRUE(artifact);
    const auto code = artifact->Bytecode();
    vector<byte> unaligned(code.size() + 1);
    std::memcpy(unaligned.data() + 1, code.data(), code.size());
    auto shader = context.Device->CreateShader({std::span{unaligned}.subspan(1), *GetShaderBlobCategory(*target), ShaderStage::Compute});
    ASSERT_TRUE(shader);
    EXPECT_EQ(shader->GetStages(), ShaderStage::Compute);
    EXPECT_EQ(context.RhiValidationErrors.load(), 0u);
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}
INSTANTIATE_TEST_SUITE_P(BackendsAndModes, RhiValidationGpuTest, testing::Combine(testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan), testing::Bool()));

}  // namespace
}  // namespace radray::render::test
