#include <radray/runtime/gpu_system.h>

#include <radray/basic_math.h>
#include <radray/file.h>
#include <radray/hash.h>
#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/render/shader_compiler/spvc.h>
#include <radray/vertex_data.h>
#include <radray/runtime/application.h>
#include <radray/runtime/window_manager.h>

#include <algorithm>
#include <cstring>
#include <type_traits>

namespace radray {

namespace {

string BuildDefineString(const ShaderDefine& define) {
    if (define.Value.empty()) {
        return define.Name;
    }
    return fmt::format("{}={}", define.Name, define.Value);
}

class HashKeyBuilder {
public:
    template <class T>
    void AddPod(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        AddPodFields(value);
    }

    template <class... Ts>
    void AddPodFields(const Ts&... values) {
        static_assert((std::is_trivially_copyable_v<Ts> && ...));
        constexpr size_t totalSize = (sizeof(Ts) + ... + 0);
        if constexpr (totalSize == 0) {
            return;
        } else {
            const size_t offset = _bytes.size();
            _bytes.resize(offset + totalSize);

            byte* cursor = _bytes.data() + offset;
            auto appendOne = [&]<class T>(const T& value) {
                std::memcpy(cursor, &value, sizeof(T));
                cursor += sizeof(T);
            };
            (appendOne(values), ...);
        }
    }

    template <class T>
    void AddEnum(T value)
    requires std::is_enum_v<T>
    {
        AddPod(EnumBits(value));
    }

    void AddString(std::string_view value) {
        AddSize(value.size());
        AddBytes(value.data(), value.size());
    }

    void AddSize(size_t value) {
        AddPod(static_cast<uint64_t>(value));
    }

    void AddBool(bool value) {
        AddPod(BoolByte(value));
    }

    void AddFloat(float value) {
        AddPod(CanonicalFloat(value));
    }

    void AddShaderDefine(const ShaderDefine& value) {
        AddString(value.Name);
        AddString(value.Value);
    }

    void AddBlendComponent(const render::BlendComponent& value) {
        AddPodFields(
            EnumBits(value.Src),
            EnumBits(value.Dst),
            EnumBits(value.Op));
    }

    void AddBlendState(const render::BlendState& value) {
        AddBlendComponent(value.Color);
        AddBlendComponent(value.Alpha);
    }

    void AddStencilFaceState(const render::StencilFaceState& value) {
        AddPodFields(
            EnumBits(value.Compare),
            EnumBits(value.FailOp),
            EnumBits(value.DepthFailOp),
            EnumBits(value.PassOp));
    }

    void AddStencilState(const render::StencilState& value) {
        AddStencilFaceState(value.Front);
        AddStencilFaceState(value.Back);
        AddPodFields(value.ReadMask, value.WriteMask);
    }

    void AddDepthBiasState(const render::DepthBiasState& value) {
        AddPodFields(
            value.Constant,
            CanonicalFloat(value.SlopScale),
            CanonicalFloat(value.Clamp));
    }

    void AddDepthStencilState(const render::DepthStencilState& value) {
        AddPodFields(
            EnumBits(value.Format),
            EnumBits(value.DepthCompare));
        AddDepthBiasState(value.DepthBias);
        AddPod(BoolByte(value.Stencil.has_value()));
        if (value.Stencil.has_value()) {
            AddStencilState(value.Stencil.value());
        }
        AddPod(BoolByte(value.DepthWriteEnable));
    }

    void AddVertexElementKey(const PSOCache::VertexElementKey& value) {
        AddPodFields(value.Offset);
        AddString(value.Semantic);
        AddPodFields(
            value.SemanticIndex,
            EnumBits(value.Format),
            value.Location);
    }

    void AddVertexLayoutKey(const PSOCache::VertexLayoutKey& value) {
        AddPodFields(
            value.ArrayStride,
            EnumBits(value.StepMode));
        AddSize(value.Elements.size());
        for (const PSOCache::VertexElementKey& element : value.Elements) {
            AddVertexElementKey(element);
        }
    }

    void AddShaderVariantKey(const ShaderVariantKey& value) {
        AddSize(value.Defines().size());
        for (const ShaderDefine& define : value.Defines()) {
            AddShaderDefine(define);
        }
    }

    void AddShaderCompileKey(const ShaderCompileKey& value) {
        AddString(value.Name);
        AddString(value.EntryPoint);
        AddPodFields(
            EnumBits(value.Stage),
            EnumBits(value.Backend));
        AddShaderVariantKey(value.Variant);
    }

    void AddResourceBindingAbi(const render::ResourceBindingAbi& value) {
        AddPodFields(
            value.Set.Value,
            value.Binding,
            EnumBits(value.Type),
            value.Count,
            BoolByte(value.IsReadOnly),
            BoolByte(value.IsBindless));
    }

    void AddPushConstantBindingAbi(const render::PushConstantBindingAbi& value) {
        AddPodFields(value.Offset, value.Size);
    }

    void AddBindingParameterLayout(const render::BindingParameterLayout& value) {
        AddString(value.Name);
        AddPodFields(
            value.Id.Value,
            EnumBits(value.Kind),
            value.Stages.value(),
            static_cast<uint32_t>(value.Abi.index()));
        if (const auto* resource = std::get_if<render::ResourceBindingAbi>(&value.Abi)) {
            AddResourceBindingAbi(*resource);
        } else if (const auto* pushConstant = std::get_if<render::PushConstantBindingAbi>(&value.Abi)) {
            AddPushConstantBindingAbi(*pushConstant);
        }
    }

    void AddPushConstantRange(const render::PushConstantRange& value) {
        AddString(value.Name);
        AddPodFields(
            value.Id.Value,
            value.Stages.value(),
            value.Offset,
            value.Size);
    }

    void AddBindlessSetLayout(const render::BindlessSetLayout& value) {
        AddString(value.Name);
        AddPodFields(
            value.Id.Value,
            value.Set.Value,
            value.Binding,
            EnumBits(value.Type),
            EnumBits(value.SlotType),
            value.Stages.value());
    }

    void AddSamplerDescriptor(const render::SamplerDescriptor& value) {
        AddPodFields(
            EnumBits(value.AddressS),
            EnumBits(value.AddressT),
            EnumBits(value.AddressR),
            EnumBits(value.MinFilter),
            EnumBits(value.MagFilter),
            EnumBits(value.MipmapFilter),
            CanonicalFloat(value.LodMin),
            CanonicalFloat(value.LodMax),
            BoolByte(value.Compare.has_value()));
        if (value.Compare.has_value()) {
            AddEnum(value.Compare.value());
        }
        AddPod(value.AnisotropyClamp);
    }

    void AddStaticSamplerLayout(const render::StaticSamplerLayout& value) {
        AddString(value.Name);
        AddPodFields(
            value.Id.Value,
            value.Set.Value,
            value.Binding,
            value.Stages.value());
        AddSamplerDescriptor(value.Desc);
    }

    void AddRootSignatureLayoutKey(const RootSignatureLayoutKey& value) {
        AddPodFields(value.DescriptorSetCount);

        AddSize(value.Parameters.size());
        for (const render::BindingParameterLayout& parameter : value.Parameters) {
            AddBindingParameterLayout(parameter);
        }

        AddSize(value.PushConstantRanges.size());
        for (const render::PushConstantRange& range : value.PushConstantRanges) {
            AddPushConstantRange(range);
        }

        AddSize(value.BindlessSetLayouts.size());
        for (const render::BindlessSetLayout& bindless : value.BindlessSetLayouts) {
            AddBindlessSetLayout(bindless);
        }

        AddSize(value.StaticSamplerLayouts.size());
        for (const render::StaticSamplerLayout& sampler : value.StaticSamplerLayouts) {
            AddStaticSamplerLayout(sampler);
        }
    }

    void AddPrimitiveState(const render::PrimitiveState& value) {
        AddPodFields(
            EnumBits(value.Topology),
            EnumBits(value.FaceClockwise),
            EnumBits(value.Cull),
            EnumBits(value.Poly),
            BoolByte(value.StripIndexFormat.has_value()));
        if (value.StripIndexFormat.has_value()) {
            AddEnum(value.StripIndexFormat.value());
        }
        AddPodFields(
            BoolByte(value.UnclippedDepth),
            BoolByte(value.Conservative));
    }

    void AddMultiSampleState(const render::MultiSampleState& value) {
        AddPodFields(
            value.Count,
            value.Mask,
            BoolByte(value.AlphaToCoverageEnable));
    }

    void AddColorTargetState(const render::ColorTargetState& value) {
        AddPodFields(
            EnumBits(value.Format),
            BoolByte(value.Blend.has_value()));
        if (value.Blend.has_value()) {
            AddBlendState(value.Blend.value());
        }
        AddPod(value.WriteMask.value());
    }

    size_t Finish() const noexcept {
        return HashData(_bytes.data(), _bytes.size());
    }

private:
    template <class T>
    static constexpr std::underlying_type_t<T> EnumBits(T value) noexcept
    requires std::is_enum_v<T>
    {
        return static_cast<std::underlying_type_t<T>>(value);
    }

    static constexpr uint8_t BoolByte(bool value) noexcept {
        return value ? 1u : 0u;
    }

    static constexpr float CanonicalFloat(float value) noexcept {
        return value == 0.0f ? 0.0f : value;
    }

    void AddBytes(const void* data, size_t size) {
        if (size == 0) {
            return;
        }
        const size_t offset = _bytes.size();
        _bytes.resize(offset + size);
        std::memcpy(_bytes.data() + offset, data, size);
    }

    vector<byte> _bytes;
};

std::optional<uint64_t> GetSubresourceUploadSize(
    const render::TextureDescriptor& desc,
    const render::SubresourceRange& range,
    std::span<const byte> srcData,
    uint64_t srcBaseRowPitch,
    uint64_t dstRowPitch) noexcept {
    const bool is3D = desc.Dim == render::TextureDimension::Dim3D;
    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(desc.Format);
    const uint32_t mipLevel = range.BaseMipLevel;
    const uint32_t mipWidth = std::max(desc.Width >> mipLevel, 1u);
    const uint32_t mipHeight = std::max(desc.Height >> mipLevel, 1u);
    const uint32_t mipDepth = is3D ? std::max(desc.DepthOrArraySize >> mipLevel, 1u) : 1u;
    const uint64_t rowBytes = static_cast<uint64_t>(mipWidth) * bytesPerPixel;
    if (srcBaseRowPitch < rowBytes) {
        return std::nullopt;
    }

    const uint64_t rowCount = static_cast<uint64_t>(mipHeight) * mipDepth;
    const uint64_t srcSize = rowCount == 0 ? 0 : (rowCount - 1) * srcBaseRowPitch + rowBytes;
    if (srcSize > srcData.size()) {
        return std::nullopt;
    }
    return dstRowPitch * rowCount;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════
//  FrameUploadScheduler
// ═══════════════════════════════════════════════════════════════

void FrameUploadStopCallback::operator()() const noexcept {
    if (Scheduler != nullptr && Record != nullptr) {
        Scheduler->CancelRecord(Record);
    }
}

FrameUploadScheduler::~FrameUploadScheduler() noexcept {
    while (!_uploads.empty()) {
        FrameUploadRecord* rec = _uploads.back().get();
        if (rec == nullptr) {
            _uploads.pop_back();
            continue;
        }
        rec->Canceled = true;
        ResumeRecord(rec);
        if (IsUploadAlive(rec)) {
            EraseUpload(rec);
        }
    }
}

task<FrameUploadScope> FrameUploadScheduler::BeginUpload() {
    stop_token stop = co_await CurrentStopToken();
    std::optional<FrameUploadScope> frame = co_await BeginFrameUploadAwaitable{this, stop};
    if (!frame.has_value()) {
        co_await StopCurrentTask();
        co_return FrameUploadScope{};
    }
    co_return frame.value();
}

FrameUploadRecord* FrameUploadScheduler::RegisterUpload(stop_token stop, std::coroutine_handle<> continuation) {
    auto rec = make_unique<FrameUploadRecord>();
    rec->Scheduler = this;
    rec->Continuation = continuation;
    rec->Stop = stop;
    rec->CurrentStage = FrameUploadStage::AwaitingFrame;
    FrameUploadRecord* ptr = rec.get();
    _uploads.emplace_back(std::move(rec));
    if (stop.stop_requested()) {
        ptr->Canceled = true;
    } else if (stop.stop_possible()) {
        ptr->StopCallback.emplace(stop, FrameUploadStopCallback{this, ptr});
    }
    return ptr;
}

bool FrameUploadScheduler::EraseUpload(FrameUploadRecord* record) noexcept {
    for (size_t i = 0; i < _uploads.size(); ++i) {
        if (_uploads[i].get() == record) {
            _uploads[i]->StopCallback.reset();
            _uploads.erase(_uploads.begin() + static_cast<ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool FrameUploadScheduler::IsUploadAlive(FrameUploadRecord* record) const noexcept {
    for (const auto& rec : _uploads) {
        if (rec.get() == record) {
            return true;
        }
    }
    return false;
}

void FrameUploadScheduler::ResumeRecord(FrameUploadRecord* record) {
    if (record == nullptr) {
        return;
    }
    std::coroutine_handle<> continuation = record->Continuation;
    record->Continuation = {};
    if (continuation) {
        continuation.resume();
    }
}

void FrameUploadScheduler::CancelRecord(FrameUploadRecord* record) noexcept {
    if (record == nullptr) {
        return;
    }
    record->Canceled = true;
    ResumeRecord(record);
}

void FrameUploadScheduler::RunUploadPhase(
    render::CommandBuffer* cmdBuffer,
    ResourceUploader& uploader,
    uint32_t flightIndex) {
    vector<FrameUploadRecord*> pending;
    for (auto& recPtr : _uploads) {
        if (recPtr->CurrentStage == FrameUploadStage::AwaitingFrame) {
            pending.push_back(recPtr.get());
        }
    }

    for (FrameUploadRecord* rec : pending) {
        if (rec->Canceled || rec->Stop.stop_requested()) {
            rec->Canceled = true;
            ResumeRecord(rec);
            if (IsUploadAlive(rec)) {
                EraseUpload(rec);
            }
            continue;
        }

        rec->Cmd = cmdBuffer;
        rec->Uploader = &uploader;
        rec->FlightIndex = flightIndex;
        rec->CurrentStage = FrameUploadStage::InFrame;

        ResumeRecord(rec);

        if (!IsUploadAlive(rec)) {
            continue;
        }
        if (rec->CurrentStage != FrameUploadStage::AwaitingFence) {
            EraseUpload(rec);
            continue;
        }
    }
}

void FrameUploadScheduler::NotifyFlightComplete(uint32_t flightIndex) {
    for (auto& recPtr : _uploads) {
        FrameUploadRecord* rec = recPtr.get();
        if (rec->CurrentStage == FrameUploadStage::AwaitingFence && rec->FlightIndex == flightIndex) {
            rec->CurrentStage = FrameUploadStage::FenceComplete;
        }
    }
}

void FrameUploadScheduler::PumpCompletedUploads() {
    bool resumedAny = true;
    while (resumedAny) {
        resumedAny = false;
        for (size_t i = 0; i < _uploads.size();) {
            FrameUploadRecord* rec = _uploads[i].get();
            if (rec->Stop.stop_requested()) {
                rec->Canceled = true;
            }
            if (rec->Canceled || rec->CurrentStage == FrameUploadStage::FenceComplete) {
                ResumeRecord(rec);
                if (IsUploadAlive(rec)) {
                    EraseUpload(rec);
                }
                resumedAny = true;
                break;
            }
            ++i;
        }
    }
}

bool WaitFrameUploadGpuAwaitable::await_ready() noexcept {
    if (_record == nullptr) {
        return true;
    }
    if (_record->Stop.stop_requested()) {
        _record->Canceled = true;
    }
    return _record->Canceled || _record->CurrentStage == FrameUploadStage::FenceComplete;
}

bool WaitFrameUploadGpuAwaitable::await_suspend(std::coroutine_handle<> h) noexcept {
    if (_record == nullptr) {
        return false;
    }
    if (_record->Stop.stop_requested()) {
        _record->Canceled = true;
        return false;
    }
    _record->Continuation = h;
    _record->CurrentStage = FrameUploadStage::AwaitingFence;
    return true;
}

bool WaitFrameUploadGpuAwaitable::await_resume() noexcept {
    if (_record == nullptr) {
        return false;
    }
    FrameUploadScheduler* scheduler = _record->Scheduler;
    bool completed = !_record->Canceled && _record->CurrentStage == FrameUploadStage::FenceComplete;
    if (scheduler != nullptr) {
        scheduler->EraseUpload(_record);
    }
    _record = nullptr;
    return completed;
}

render::CommandBuffer* FrameUploadScope::GetCommandBuffer() const noexcept {
    return _record->Cmd;
}

ResourceUploader& FrameUploadScope::GetUploader() const noexcept {
    return *_record->Uploader;
}

uint32_t FrameUploadScope::GetFlightIndex() const noexcept {
    return _record->FlightIndex;
}

task<void> FrameUploadScope::WaitGpu() {
    bool completed = co_await WaitFrameUploadGpuAwaitable{_record};
    if (!completed) {
        co_await StopCurrentTask();
    }
}

bool BeginFrameUploadAwaitable::await_ready() const noexcept {
    return _scheduler == nullptr || _stop.stop_requested();
}

bool BeginFrameUploadAwaitable::await_suspend(std::coroutine_handle<> h) {
    if (_scheduler == nullptr || _stop.stop_requested()) {
        return false;
    }
    _record = _scheduler->RegisterUpload(_stop, h);
    return true;
}

std::optional<FrameUploadScope> BeginFrameUploadAwaitable::await_resume() noexcept {
    if (_record == nullptr) {
        return std::nullopt;
    }
    if (_record->Canceled || _record->Stop.stop_requested()) {
        _record->Canceled = true;
        FrameUploadScheduler* scheduler = _record->Scheduler;
        if (scheduler != nullptr) {
            scheduler->EraseUpload(_record);
        }
        _record = nullptr;
        return std::nullopt;
    }
    return FrameUploadScope{_record};
}

// ═══════════════════════════════════════════════════════════════
//  GpuSystem
// ═══════════════════════════════════════════════════════════════

void DeferredRenderDeleteQueue::Push(uint64_t targetFenceValue, unique_ptr<render::RenderBase> obj) noexcept {
    if (obj == nullptr) {
        return;
    }
    std::lock_guard lock{_mutex};
    _entries.emplace_back(DeferredRenderDeleteEntry{
        .TargetFenceValue = targetFenceValue,
        .Object = std::move(obj)});
}

void DeferredRenderDeleteQueue::Process(uint64_t completedFenceValue) noexcept {
    std::lock_guard lock{_mutex};
    std::erase_if(
        _entries,
        [completedFenceValue](const DeferredRenderDeleteEntry& entry) noexcept {
            return entry.TargetFenceValue <= completedFenceValue;
        });
}

void DeferredRenderDeleteQueue::Flush() noexcept {
    std::lock_guard lock{_mutex};
    _entries.clear();
}

uint32_t DeferredRenderDeleteQueue::Count() const noexcept {
    std::lock_guard lock{_mutex};
    return static_cast<uint32_t>(_entries.size());
}

bool GpuSystem::CompleteFlight(uint32_t flightIndex) {
    auto& flight = _flights[flightIndex];
    if (!flight.Signal.IsValid() || flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
        return false;
    }

    _lastFrameLatency = std::chrono::steady_clock::now() - flight.FrameStartTime;
    flight.Signal = GpuSystem::FenceSignal::Invalid();
    if (_frameProfiler != nullptr) {
        _frameProfiler->Resolve(flightIndex);
    }
    _app->NotifyRenderComplete(AppRenderCompleteContext{.FlightIndex = flightIndex});
    flight.WaitForDestroy.clear();
    _uploader->CollectFlight(flightIndex);
    // 该 flight 的 fence 已完成:标记等在这个 flight 上的上传记录(供下次 scheduler pump 恢复加载协程)。
    if (_frameUploadScheduler != nullptr) {
        _frameUploadScheduler->NotifyFlightComplete(flightIndex);
    }
    ProcessDeferredDeletes();
    return true;
}

bool GpuSystem::CompleteFlightIfReady(uint32_t flightIndex, bool wait) {
    if (flightIndex >= _flights.size()) {
        return false;
    }

    FlightSlot& flight = _flights[flightIndex];
    if (!flight.Signal.IsValid()) {
        return true;
    }
    if (wait) {
        flight.Signal.Fence->Wait(flight.Signal.Value);
    } else if (flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
        return false;
    }
    return CompleteFlight(flightIndex);
}

void GpuSystem::BeginUpdateForFlight(uint32_t flightIndex) {
    if (flightIndex >= _flights.size()) {
        return;
    }

    FlightSlot& flight = _flights[flightIndex];
    flight.WaitForDestroy.clear();
    flight.FrameStartTime = std::chrono::steady_clock::now();
}

// ═════════════════════════════════════════════════════════════════
//  GpuFrameProfiler
// ═════════════════════════════════════════════════════════════════

GpuFrameProfiler::GpuFrameProfiler(render::Device* device, render::CommandQueue* queue, uint32_t flightCount)
    : _queue(queue) {
    // Vulkan 需要在 readback copy 前后显式 transition;D3D12 READBACK heap 始终处于 COPY_DEST。
    _readbackNeedsBarrier = device->GetBackend() == render::RenderBackend::Vulkan;
    _frames.resize(flightCount);
    for (FrameTiming& frame : _frames) {
        render::QueryPoolDescriptor poolDesc{
            .Type = render::QueryType::Timestamp,
            .Count = TimestampQueryCount,
            .DebugName = "GpuFrameProfiler Timestamp Pool"};
        frame.Pool = device->CreateQueryPool(poolDesc).Unwrap();

        render::BufferDescriptor readbackDesc{
            .Size = sizeof(uint64_t) * TimestampQueryCount,
            .Memory = render::MemoryType::ReadBack,
            .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead};
        frame.Readback = device->CreateBuffer(readbackDesc).Unwrap();
    }
}

GpuFrameProfiler::~GpuFrameProfiler() noexcept = default;

void GpuFrameProfiler::BeginFrame(render::CommandBuffer* cmdBuffer, uint32_t flightIndex) {
    if (cmdBuffer == nullptr || flightIndex >= _frames.size()) {
        return;
    }
    FrameTiming& frame = _frames[flightIndex];
    cmdBuffer->ResetQueryPool(frame.Pool.get(), 0, TimestampQueryCount);
    cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
        .Pool = frame.Pool.get(),
        .Stage = render::QueryPipelineStage::Top,
        .Index = 0});
}

void GpuFrameProfiler::EndFrame(render::CommandBuffer* cmdBuffer, uint32_t flightIndex) {
    if (cmdBuffer == nullptr || flightIndex >= _frames.size()) {
        return;
    }
    FrameTiming& frame = _frames[flightIndex];
    cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
        .Pool = frame.Pool.get(),
        .Stage = render::QueryPipelineStage::Bottom,
        .Index = 1});
    if (_readbackNeedsBarrier) {
        render::ResourceBarrierDescriptor toCopyDst = render::BarrierBufferDescriptor{
            .Target = frame.Readback.get(),
            .Before = render::BufferState::Common,
            .After = render::BufferState::CopyDestination};
        cmdBuffer->ResourceBarrier(std::span{&toCopyDst, 1});
    }
    cmdBuffer->ResolveQueryData(render::QueryResolveDescriptor{
        .Pool = frame.Pool.get(),
        .FirstIndex = 0,
        .Count = TimestampQueryCount,
        .Destination = frame.Readback.get(),
        .DestinationOffset = 0});
    if (_readbackNeedsBarrier) {
        render::ResourceBarrierDescriptor toHostRead = render::BarrierBufferDescriptor{
            .Target = frame.Readback.get(),
            .Before = render::BufferState::CopyDestination,
            .After = render::BufferState::HostRead};
        cmdBuffer->ResourceBarrier(std::span{&toHostRead, 1});
    }
    frame.Pending = true;
}

void GpuFrameProfiler::Resolve(uint32_t flightIndex) {
    if (flightIndex >= _frames.size()) {
        return;
    }
    FrameTiming& frame = _frames[flightIndex];
    if (!frame.Pending) {
        return;
    }
    frame.Pending = false;

    const uint64_t mappedSize = sizeof(uint64_t) * TimestampQueryCount;
    void* mapped = frame.Readback->Map(0, mappedSize);
    if (mapped == nullptr) {
        return;
    }
    uint64_t ticks[TimestampQueryCount]{};
    std::memcpy(ticks, mapped, mappedSize);
    frame.Readback->Unmap(0, mappedSize);

    if (ticks[1] <= ticks[0]) {
        return;
    }
    const render::TimestampQueryCalibration calibration = frame.Pool->GetTimestampCalibration(_queue);
    if (calibration.TickPeriodNs <= 0.0) {
        return;
    }
    const double elapsedNs = static_cast<double>(ticks[1] - ticks[0]) * calibration.TickPeriodNs;
    _lastGpuTimeMs = static_cast<float>(elapsedNs / 1'000'000.0);
}

// ═════════════════════════════════════════════════════════════════
//  GpuSystem
// ═════════════════════════════════════════════════════════════════

GpuSystem::GpuSystem(Application* app, const GpuSystemDescriptor& desc)
    : _app(app),
      _device(desc.Device),
      _mainQueue(desc.Device->GetCommandQueue(render::QueueType::Direct, desc.MainQueueIndex).Unwrap()),
      _backBufferCount(desc.BackBufferCount),
      _flightDataCount(desc.FlightDataCount) {
    _mainQueueTrack.Queue = _mainQueue;
    _mainQueueTrack.Fence = _device->CreateFence().Unwrap();
    _mainQueueTrack.Fence->SetDebugName("AppMainQueue");
    _flights.resize(_flightDataCount);
    _uploader = make_unique<ResourceUploader>(_device, _flightDataCount);
    _frameUploadScheduler = make_unique<FrameUploadScheduler>();
    // shader 编译的默认 include 根目录:约定 shaderlib 随可执行文件部署在运行时目录下。
    // 这样 shader 源码可直接 #include "common.hlsl" 等 shaderlib 头文件。
    _shaderCache = make_unique<ShaderCache>(_device, (GetExecutableDirectory() / "shaderlib").generic_string());
    _rsCache = make_unique<RSCache>(_device);
    _psoCache = make_unique<PSOCache>(_device);
    if (desc.EnableFrameProfiler) {
        _frameProfiler = make_unique<GpuFrameProfiler>(_device, _mainQueue, _flightDataCount);
    }
}

GpuSystem::~GpuSystem() noexcept {
    FlushAllDeferredDeletes();
}

void GpuSystem::RecycleRenderResource(unique_ptr<render::RenderBase> obj) noexcept {
    if (obj == nullptr) {
        return;
    }
    const uint64_t targetFenceValue = _mainQueueTrack.NextFenceValue.load(std::memory_order_acquire);
    _deferredDeletes.Push(targetFenceValue, std::move(obj));
}

void GpuSystem::ProcessDeferredDeletes() noexcept {
    if (_mainQueueTrack.Fence == nullptr) {
        return;
    }
    _deferredDeletes.Process(_mainQueueTrack.Fence->GetCompletedValue());
}

void GpuSystem::FlushAllDeferredDeletes() noexcept {
    _deferredDeletes.Flush();
}

float GpuSystem::GetLastGpuTimeMs() const noexcept {
    return _frameProfiler != nullptr ? _frameProfiler->GetLastGpuTimeMs() : 0.0f;
}

uint32_t GpuSystem::GetCurrentFlightIndex() const noexcept {
    return static_cast<uint32_t>(_nowFrameIndex % _flightDataCount);
}

void GpuSystem::PumpFrameUploadScheduler() {
    if (_frameUploadScheduler != nullptr) {
        _frameUploadScheduler->PumpCompletedUploads();
    }
}

ShaderCache::ShaderCache(render::Device* device, std::string_view shaderIncludeDir)
    : _device(device), _shaderIncludeDir(shaderIncludeDir) {}

size_t ShaderCache::ShaderCompileKeyHash::operator()(const ShaderCompileKey& key) const noexcept {
    HashKeyBuilder hash;
    hash.AddShaderCompileKey(key);
    return hash.Finish();
}

std::optional<CompiledShaderEntry> ShaderCache::GetOrCompileEntry(const ShaderCompileDescriptor& desc) {
    ShaderVariantKey variant{desc.Defines};
    ShaderCompileKey key{
        .Name = string{desc.Name},
        .EntryPoint = string{desc.EntryPoint},
        .Stage = desc.Stage,
        .Backend = _device->GetBackend(),
        .Variant = variant};
    if (auto it = _cache.find(key); it != _cache.end()) {
        return CompiledShaderEntry{
            .Target = it->second.get(),
            .Key = key};
    }

    const bool isSpirv = key.Backend == render::RenderBackend::Vulkan;

    if (_dxc == nullptr) {
        auto dxcOpt = render::CreateDxc();
        if (!dxcOpt.HasValue()) {
            RADRAY_ERR_LOG("ShaderCache: failed to create DXC compiler");
            return std::nullopt;
        }
        _dxc = dxcOpt.Release();
    }

    render::DxcCompileParams params{};
    params.Code = desc.Source;
    params.EntryPoint = desc.EntryPoint;
    params.Stage = desc.Stage;
    params.SM = render::HlslShaderModel::SM60;
    params.IsOptimize = false;
    params.IsSpirv = isSpirv;
    vector<string> defineStorage;
    vector<std::string_view> defineViews;
    defineStorage.reserve(variant.Defines().size() + 1);
    defineStorage.emplace_back(isSpirv ? "VULKAN=1" : "D3D12=1");
    for (const ShaderDefine& define : variant.Defines()) {
        defineStorage.emplace_back(BuildDefineString(define));
    }
    defineViews.reserve(defineStorage.size());
    for (const string& define : defineStorage) {
        defineViews.emplace_back(define);
    }
    params.Defines = defineViews;
    // 默认注入 <exe_dir>/shaderlib 作为 include 根目录，shader 可直接 #include "common.hlsl" 等。
    // span 指向的存储须活到 Compile 返回，故放在同作用域的栈上。
    std::string_view includeDirs[1];
    if (!_shaderIncludeDir.empty()) {
        includeDirs[0] = _shaderIncludeDir;
        params.Includes = std::span<std::string_view>{includeDirs, 1};
    }
    auto outputOpt = _dxc->Compile(params);
    if (!outputOpt.has_value()) {
        RADRAY_ERR_LOG("ShaderCache: failed to compile shader '{}' entry '{}'", desc.Name, desc.EntryPoint);
        return std::nullopt;
    }
    auto output = std::move(outputOpt.value());

    render::ShaderReflectionDesc reflection{};
    render::ShaderBlobCategory category{};
    if (isSpirv) {
#ifdef RADRAY_ENABLE_SPIRV_CROSS
        auto reflOpt = render::ReflectSpirv(render::SpirvBytecodeView{
            .Data = output.Data,
            .EntryPointName = desc.EntryPoint,
            .Stage = desc.Stage});
        if (!reflOpt.has_value()) {
            RADRAY_ERR_LOG("ShaderCache: failed to reflect SPIR-V shader '{}'", desc.Name);
            return std::nullopt;
        }
        reflection = std::move(reflOpt.value());
        category = render::ShaderBlobCategory::SPIRV;
#else
        RADRAY_ERR_LOG("ShaderCache: SPIR-V Cross reflection is not enabled in this build");
        return std::nullopt;
#endif
    } else {
        auto reflOpt = _dxc->GetShaderDescFromOutput(output.Refl);
        if (!reflOpt.has_value()) {
            RADRAY_ERR_LOG("ShaderCache: failed to reflect DXIL shader '{}'", desc.Name);
            return std::nullopt;
        }
        reflection = std::move(reflOpt.value());
        category = render::ShaderBlobCategory::DXIL;
    }

    render::ShaderDescriptor shaderDesc{};
    shaderDesc.Source = std::span<const byte>{output.Data.data(), output.Data.size()};
    shaderDesc.Category = category;
    shaderDesc.Stages = desc.Stage;
    shaderDesc.Reflection = std::move(reflection);
    auto shaderOpt = _device->CreateShader(shaderDesc);
    if (!shaderOpt.HasValue()) {
        RADRAY_ERR_LOG("ShaderCache: failed to create shader '{}'", desc.Name);
        return std::nullopt;
    }
    render::Shader* raw = shaderOpt.Get();
    _cache.emplace(std::move(key), shaderOpt.Release());
    return CompiledShaderEntry{
        .Target = raw,
        .Key = ShaderCompileKey{
            .Name = string{desc.Name},
            .EntryPoint = string{desc.EntryPoint},
            .Stage = desc.Stage,
            .Backend = _device->GetBackend(),
            .Variant = ShaderVariantKey{desc.Defines}}};
}

Nullable<render::Shader*> ShaderCache::GetOrCompile(const ShaderCompileDescriptor& desc) {
    std::optional<CompiledShaderEntry> entry = GetOrCompileEntry(desc);
    if (!entry.has_value()) {
        return nullptr;
    }
    return entry->Target;
}

Nullable<render::Shader*> ShaderCache::GetOrCompileFromFile(
    const std::filesystem::path& path,
    std::string_view entryPoint,
    render::ShaderStage stage,
    std::string_view name,
    std::span<const ShaderDefine> defines) {
    auto sourceOpt = ReadTextFile(path);
    if (!sourceOpt.has_value()) {
        RADRAY_ERR_LOG("ShaderCache: failed to read shader file {}", path.string());
        return nullptr;
    }
    string pathName = name.empty() ? path.string() : string{name};
    ShaderCompileDescriptor desc{};
    desc.Name = pathName;
    desc.Source = sourceOpt.value();
    desc.EntryPoint = entryPoint;
    desc.Stage = stage;
    desc.Defines = defines;
    return GetOrCompile(desc);
}

std::optional<CompiledShaderEntry> ShaderCache::GetOrCompileEntryFromFile(
    const std::filesystem::path& path,
    std::string_view entryPoint,
    render::ShaderStage stage,
    std::string_view name,
    std::span<const ShaderDefine> defines) {
    auto sourceOpt = ReadTextFile(path);
    if (!sourceOpt.has_value()) {
        RADRAY_ERR_LOG("ShaderCache: failed to read shader file {}", path.string());
        return std::nullopt;
    }
    string pathName = name.empty() ? path.string() : string{name};
    ShaderCompileDescriptor desc{};
    desc.Name = pathName;
    desc.Source = sourceOpt.value();
    desc.EntryPoint = entryPoint;
    desc.Stage = stage;
    desc.Defines = defines;
    return GetOrCompileEntry(desc);
}

std::optional<CompiledShaderEntry> GpuSystem::GetOrCompileShaderEntry(const ShaderCompileDescriptor& desc) {
    return _shaderCache->GetOrCompileEntry(desc);
}

Nullable<render::Shader*> GpuSystem::GetOrCompileShader(const ShaderCompileDescriptor& desc) {
    return _shaderCache->GetOrCompile(desc);
}

Nullable<render::Shader*> GpuSystem::GetOrCompileShaderFromFile(
    const std::filesystem::path& path,
    std::string_view entryPoint,
    render::ShaderStage stage,
    std::string_view name,
    std::span<const ShaderDefine> defines) {
    return _shaderCache->GetOrCompileFromFile(path, entryPoint, stage, name, defines);
}

std::optional<CompiledShaderEntry> GpuSystem::GetOrCompileShaderEntryFromFile(
    const std::filesystem::path& path,
    std::string_view entryPoint,
    render::ShaderStage stage,
    std::string_view name,
    std::span<const ShaderDefine> defines) {
    return _shaderCache->GetOrCompileEntryFromFile(path, entryPoint, stage, name, defines);
}

Nullable<render::RootSignature*> GpuSystem::GetOrCreateRootSignature(std::span<render::Shader*> shaders) {
    for (render::Shader* shader : shaders) {
        if (shader == nullptr) {
            RADRAY_ERR_LOG("GpuSystem: cannot create root signature from null shader");
            return nullptr;
        }
    }
    return _rsCache->GetOrCreate(shaders);
}

std::optional<RootSignatureEntry> GpuSystem::GetOrCreateRootSignatureEntry(std::span<render::Shader*> shaders) {
    for (render::Shader* shader : shaders) {
        if (shader == nullptr) {
            RADRAY_ERR_LOG("GpuSystem: cannot create root signature from null shader");
            return std::nullopt;
        }
    }
    return _rsCache->GetOrCreateEntry(shaders);
}

void GpuSystem::WaitAndCleanupCompletedFlights() {
    _mainQueue->Wait();

    for (uint32_t flightIndex = 0; flightIndex < _flights.size(); ++flightIndex) {
        CompleteFlight(flightIndex);
    }
}

AppFrameContext GpuSystem::BeginFrameRecord(
    uint32_t flightIndex,
    std::chrono::duration<float> deltaTime,
    std::chrono::duration<float> lastFrameLatency,
    bool isInModalLoop) {
    FlightSlot& record = _flights[flightIndex];
    if (record.CmdBuffer == nullptr) {
        record.CmdBuffer = _device->CreateCommandBuffer(_mainQueue).Unwrap();
    }
    record.Targets.clear();
    record.ManualSubmit = false;
    record.Recording = true;
    record.CmdBuffer->Begin();
    // 帧顶(任何 RenderPass 之前、裸 CommandBuffer):交付本帧 cmd/uploader/flight 给等在
    // GPU 上传点的加载协程并 inline 恢复,让它们在本帧默认 cmdbuffer 上录制 copy。
    // copy 与本帧绘制同一提交,fence 完成后由 CompleteFlight 推进加载协程。
    if (_frameUploadScheduler != nullptr) {
        _frameUploadScheduler->RunUploadPhase(record.CmdBuffer.get(), *_uploader, flightIndex);
    }
    if (_frameProfiler != nullptr) {
        _frameProfiler->BeginFrame(record.CmdBuffer.get(), flightIndex);
    }
    return AppFrameContext{this, flightIndex, deltaTime, lastFrameLatency, isInModalLoop};
}

void GpuSystem::EndFrameRecordAndSubmit(uint32_t flightIndex) {
    FlightSlot& record = _flights[flightIndex];
    if (!record.Recording) {
        return;
    }
    record.Recording = false;

    // 闭合上传链路：本帧录制的 staging + AssetRef 绑定到该 flight。
    _uploader->EndFlight(flightIndex);

    // 帧尾 GPU 耗时收尾:写 Bottom timestamp + resolve 到 readback(在 End 之前、后续 barrier 之后录制)。
    if (_frameProfiler != nullptr) {
        _frameProfiler->EndFrame(record.CmdBuffer.get(), flightIndex);
    }

    record.CmdBuffer->End();

    // ManualSubmit：应用已自行 submit/present，runtime 仅负责 End 与后续回收。
    if (record.ManualSubmit) {
        record.Targets.clear();
        return;
    }

    // 聚合全部 target 的同步对象。
    vector<render::SwapChainSyncObject*> waitToExecute;
    vector<render::SwapChainSyncObject*> readyToPresent;
    waitToExecute.reserve(record.Targets.size());
    readyToPresent.reserve(record.Targets.size());
    for (FlightSlot::AcquiredTarget& target : record.Targets) {
        if (render::SwapChainSyncObject* syncObject = target.Frame.GetWaitToDraw()) {
            waitToExecute.emplace_back(syncObject);
        }
        if (render::SwapChainSyncObject* syncObject = target.Frame.GetReadyToPresent()) {
            readyToPresent.emplace_back(syncObject);
        }
    }

    render::Fence* frameFence = _mainQueueTrack.Fence.get();
    render::CommandBuffer* submitCmdBuffers[] = {record.CmdBuffer.get()};
    render::Fence* signalFences[] = {frameFence};
    uint64_t signalValues[] = {_mainQueueTrack.NextFenceValue.fetch_add(1, std::memory_order_acq_rel)};
    render::CommandQueueSubmitDescriptor submitDesc{
        .CmdBuffers = std::span{submitCmdBuffers},
        .SignalFences = std::span{signalFences},
        .SignalValues = std::span{signalValues},
        .WaitToExecute = std::span{waitToExecute},
        .ReadyToPresent = std::span{readyToPresent}};
    _mainQueue->Submit(submitDesc);
    _flights[flightIndex].Signal = GpuSystem::FenceSignal{
        .Fence = frameFence,
        .Value = signalValues[0]};

    // 逐个呈现。RequireRecreate 静默跳过，其余非 Success 记日志。
    for (FlightSlot::AcquiredTarget& target : record.Targets) {
        render::SwapChainPresentResult present =
            target.Window->PresentSwapChainFrame(std::move(target.Frame));
        if (present.Status == render::SwapChainStatus::RequireRecreate) {
            continue;
        }
        if (present.Status != render::SwapChainStatus::Success) {
            RADRAY_ERR_LOG("failed to present swapchain frame: status={}, native={}", present.Status, present.NativeStatusCode);
        }
    }
    record.Targets.clear();
}

// ══════════════════════════════════════════════
//  RSCache
// ══════════════════════════════════════════════

RootSignatureLayoutKey RootSignatureLayoutKey::From(
    const render::RootSignatureLayoutPreview& preview) {
    RootSignatureLayoutKey key{};
    key.DescriptorSetCount = preview.DescriptorSetCount;

    auto parameters = preview.Layout.GetParameters();
    key.Parameters.reserve(parameters.size());
    key.Parameters.assign(parameters.begin(), parameters.end());
    key.PushConstantRanges = preview.PushConstantRanges;
    key.BindlessSetLayouts = preview.BindlessSetLayouts;
    key.StaticSamplerLayouts = preview.StaticSamplerLayouts;

    return key;
}

size_t RSCache::RootSignatureLayoutKeyHash::operator()(const RootSignatureLayoutKey& key) const noexcept {
    HashKeyBuilder hash;
    hash.AddRootSignatureLayoutKey(key);
    return hash.Finish();
}

std::optional<RootSignatureEntry> RSCache::GetOrCreateEntry(std::span<render::Shader*> shaders) {
    if (shaders.empty()) {
        RADRAY_ERR_LOG("RSCache: cannot create root signature from empty shader set");
        return std::nullopt;
    }

    for (render::Shader* shader : shaders) {
        if (shader == nullptr) {
            RADRAY_ERR_LOG("RSCache: cannot create root signature from null shader");
            return std::nullopt;
        }
    }

    render::RootSignatureDescriptor rsDesc{};
    rsDesc.Shaders = shaders;

    auto previewOpt = render::BuildRootSignatureLayoutPreview(_device->GetBackend(), rsDesc);
    if (!previewOpt.has_value()) {
        RADRAY_ERR_LOG("RSCache: failed to build root signature layout preview");
        return std::nullopt;
    }

    RootSignatureLayoutKey layoutKey = RootSignatureLayoutKey::From(previewOpt.value());
    if (auto layoutIt = _layoutCache.find(layoutKey); layoutIt != _layoutCache.end()) {
        return RootSignatureEntry{
            .Target = layoutIt->second.get(),
            .Layout = std::move(layoutKey)};
    }

    auto rsOpt = _device->CreateRootSignature(rsDesc);
    if (!rsOpt.HasValue()) {
        RADRAY_ERR_LOG("RSCache: failed to create root signature");
        return std::nullopt;
    }

    render::RootSignature* raw = rsOpt.Get();
    RootSignatureEntry entry{
        .Target = raw,
        .Layout = layoutKey};
    _layoutCache.emplace(std::move(layoutKey), rsOpt.Release());
    return entry;
}

Nullable<render::RootSignature*> RSCache::GetOrCreate(std::span<render::Shader*> shaders) {
    std::optional<RootSignatureEntry> entry = GetOrCreateEntry(shaders);
    if (!entry.has_value()) {
        return nullptr;
    }
    return entry->Target;
}

// ══════════════════════════════════════════════
//  AppFrameContext
// ══════════════════════════════════════════════

render::CommandBuffer* AppFrameContext::GetCommandBuffer() const noexcept {
    return _gpuSystem->_flights[_flightIndex].CmdBuffer.get();
}

std::optional<AppFrameTarget> AppFrameContext::AcquireWindow(AppWindow* window) {
    if (window == nullptr) {
        return std::nullopt;
    }
    const AppRenderContext renderCtx{
        .FlightIndex = _flightIndex,
        .DeltaTime = _deltaTime,
        .LastFrameLatency = _lastFrameLatency,
        .IsInModalLoop = _isInModalLoop};
    render::SwapChainAcquireResult acquire = window->AcquireNextSwapChainFrame(renderCtx);
    if (acquire.Status != render::SwapChainStatus::Success || !acquire.Frame.has_value()) {
        if (acquire.Status != render::SwapChainStatus::RequireRecreate &&
            acquire.Status != render::SwapChainStatus::RetryLater) {
            RADRAY_ERR_LOG("failed to acquire swapchain frame: status={}, native={}", acquire.Status, acquire.NativeStatusCode);
        }
        return std::nullopt;
    }

    render::SwapChainFrame frame = std::move(acquire.Frame.value());
    render::Texture* backBuffer = frame.GetBackBuffer();
    const uint32_t backBufferIndex = frame.GetBackBufferIndex();
    render::TextureView* backBufferView = window->GetOrCreateBackBufferView(frame, _flightIndex);
    if (backBufferView == nullptr) {
        // 未能建立 view：丢弃该 frame（不提交）。
        return std::nullopt;
    }

    GpuSystem::FlightSlot& record = _gpuSystem->_flights[_flightIndex];
    record.Targets.emplace_back(GpuFlightAcquiredTarget{
        .Window = window,
        .Frame = std::move(frame)});
    return AppFrameTarget{
        .Window = window,
        .BackBuffer = backBuffer,
        .BackBufferView = backBufferView,
        .BackBufferIndex = backBufferIndex};
}

ResourceUploader& AppFrameContext::GetUploader() const noexcept {
    return *_gpuSystem->_uploader;
}

render::Device* AppFrameContext::GetDevice() const noexcept {
    return _gpuSystem->_device;
}

render::CommandQueue* AppFrameContext::GetMainQueue() const noexcept {
    return _gpuSystem->_mainQueue;
}

void AppFrameContext::SetManualSubmit() noexcept {
    _gpuSystem->_flights[_flightIndex].ManualSubmit = true;
}

// ═══════════════════════════════════════════════════════════════
//  StagingBufferPool
// ═══════════════════════════════════════════════════════════════

StagingBufferPool::StagingBufferPool(render::Device* device, uint32_t flightCount) noexcept
    : _device(device) {
    _pending.resize(flightCount);
}

StagingBufferPool::~StagingBufferPool() noexcept = default;

StagingBufferPool::Allocation StagingBufferPool::Allocate(uint64_t size) {
    // 尝试从 free list 找一个足够大的 buffer 复用
    for (auto it = _freeList.begin(); it != _freeList.end(); ++it) {
        if ((*it)->GetDesc().Size >= size) {
            auto buf = std::move(*it);
            _freeList.erase(it);
            void* mapped = buf->Map(0, size);
            auto* rawPtr = buf.get();
            _active.emplace_back(ActiveBuffer{
                .Buffer = std::move(buf),
                .IsMapped = true,
                .MappedSize = size});
            return Allocation{rawPtr, mapped, 0, size};
        }
    }
    // 没有合适的，创建新的 staging buffer
    render::BufferDescriptor desc{
        .Size = size,
        .Memory = render::MemoryType::Upload,
        .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite,
        .Hints = render::ResourceHint::None};
    auto bufOpt = _device->CreateBuffer(desc);
    if (!bufOpt.HasValue()) {
        RADRAY_ABORT("StagingBufferPool::Allocate failed to create buffer of size {}", size);
    }
    auto buf = bufOpt.Release();
    buf->SetDebugName(fmt::format("staging_{}", size));
    void* mapped = buf->Map(0, size);
    auto* rawPtr = buf.get();
    _active.emplace_back(ActiveBuffer{
        .Buffer = std::move(buf),
        .IsMapped = true,
        .MappedSize = size});
    return Allocation{rawPtr, mapped, 0, size};
}

void StagingBufferPool::FlushAndUnmap(const Allocation& allocation) {
    if (allocation.Buffer == nullptr) {
        return;
    }
    for (auto& active : _active) {
        if (active.Buffer.get() != allocation.Buffer || !active.IsMapped) {
            continue;
        }
        active.Buffer->Unmap(allocation.Offset, allocation.Size);
        active.IsMapped = false;
        active.MappedSize = 0;
        return;
    }
}

void StagingBufferPool::RetireToFlight(uint32_t flightIndex) {
    // 移入 pending 前确保没有仍保持映射的 staging buffer。
    for (auto& active : _active) {
        if (active.IsMapped) {
            active.Buffer->Unmap(0, active.MappedSize);
            active.IsMapped = false;
            active.MappedSize = 0;
        }
    }
    auto& pending = _pending[flightIndex];
    for (auto& active : _active) {
        pending.emplace_back(std::move(active.Buffer));
    }
    _active.clear();
}

void StagingBufferPool::CollectFlight(uint32_t flightIndex) {
    auto& pending = _pending[flightIndex];
    _freeList.insert(_freeList.end(),
                     std::make_move_iterator(pending.begin()),
                     std::make_move_iterator(pending.end()));
    pending.clear();
}

// ═══════════════════════════════════════════════════════════════
//  ResourceUploader
// ═══════════════════════════════════════════════════════════════

ResourceUploader::ResourceUploader(render::Device* device, uint32_t flightCount)
    : _device(device),
      _stagingPool(device, flightCount) {
    (void)flightCount;
}

ResourceUploader::~ResourceUploader() noexcept = default;

void ResourceUploader::UploadBuffer(
    render::CommandBuffer* cmdBuffer,
    const BufferUploadRequest& request) {
    if (request.SrcData.empty() || request.DstBuffer == nullptr) {
        return;
    }
    const uint64_t size = request.SrcData.size();
    const auto dstDesc = request.DstBuffer->GetDesc();
    if (request.DstOffset > dstDesc.Size || size > dstDesc.Size - request.DstOffset) {
        return;
    }

    // 分配 staging 并拷贝 CPU 数据
    auto alloc = _stagingPool.Allocate(size);
    std::memcpy(alloc.MappedPtr, request.SrcData.data(), size);
    _stagingPool.FlushAndUnmap(alloc);

    vector<render::ResourceBarrierDescriptor> barriersBefore;
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        barriersBefore.emplace_back(render::BarrierBufferDescriptor{
            .Target = alloc.Buffer,
            .Before = render::BufferState::HostWrite,
            .After = render::BufferState::CopySource});
    }
    barriersBefore.emplace_back(render::BarrierBufferDescriptor{
        .Target = request.DstBuffer,
        .Before = request.Before,
        .After = render::BufferState::CopyDestination});
    cmdBuffer->ResourceBarrier(barriersBefore);

    // 录制 copy
    cmdBuffer->CopyBufferToBuffer(
        request.DstBuffer, request.DstOffset,
        alloc.Buffer, alloc.Offset,
        size);

    // 录制 barrier: dst → Common
    render::ResourceBarrierDescriptor barrierAfter = render::BarrierBufferDescriptor{
        .Target = request.DstBuffer,
        .Before = render::BufferState::CopyDestination,
        .After = request.After};
    cmdBuffer->ResourceBarrier(std::span{&barrierAfter, 1});
}

void ResourceUploader::UploadTexture(
    render::CommandBuffer* cmdBuffer,
    const TextureUploadRequest& request) {
    if (request.SrcData.empty() || request.DstTexture == nullptr) {
        return;
    }
    const auto desc = request.DstTexture->GetDesc();
    const bool is3D = desc.Dim == render::TextureDimension::Dim3D;
    const uint32_t arraySize = is3D ? 1u : desc.DepthOrArraySize;
    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(desc.Format);
    if (bytesPerPixel == 0 ||
        request.DstRange.MipLevelCount != 1 ||
        request.DstRange.ArrayLayerCount != 1 ||
        request.DstRange.BaseMipLevel >= desc.MipLevels ||
        request.DstRange.BaseArrayLayer >= arraySize) {
        return;
    }

    const uint32_t baseWidth = std::max(desc.Width >> request.DstRange.BaseMipLevel, 1u);
    const uint64_t tightRowPitch = static_cast<uint64_t>(baseWidth) * bytesPerPixel;
    const uint64_t srcRowPitch = request.SrcRowPitch == 0 ? tightRowPitch : request.SrcRowPitch;
    if (srcRowPitch < tightRowPitch) {
        return;
    }
    const uint64_t dstRowPitch = Align(tightRowPitch, std::max<uint64_t>(1, _device->GetDetail().TextureDataPitchAlignment));
    auto uploadSize = GetSubresourceUploadSize(desc, request.DstRange, request.SrcData, srcRowPitch, dstRowPitch);
    if (!uploadSize.has_value()) {
        return;
    }

    // 分配 staging 并拷贝 CPU 数据
    auto alloc = _stagingPool.Allocate(uploadSize.value());
    auto* dst = static_cast<byte*>(alloc.MappedPtr);
    const auto* src = request.SrcData.data();
    const uint32_t mipLevel = request.DstRange.BaseMipLevel;
    const uint32_t mipWidth = std::max(desc.Width >> mipLevel, 1u);
    const uint32_t mipHeight = std::max(desc.Height >> mipLevel, 1u);
    const uint32_t mipDepth = is3D ? std::max(desc.DepthOrArraySize >> mipLevel, 1u) : 1u;
    const uint64_t rowBytes = static_cast<uint64_t>(mipWidth) * bytesPerPixel;
    uint64_t srcOffset = 0;
    uint64_t dstOffset = 0;
    for (uint32_t depth = 0; depth < mipDepth; ++depth) {
        for (uint32_t row = 0; row < mipHeight; ++row) {
            std::memcpy(dst + dstOffset, src + srcOffset, rowBytes);
            srcOffset += srcRowPitch;
            dstOffset += dstRowPitch;
        }
    }
    _stagingPool.FlushAndUnmap(alloc);

    vector<render::ResourceBarrierDescriptor> barriersBefore;
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        barriersBefore.emplace_back(render::BarrierBufferDescriptor{
            .Target = alloc.Buffer,
            .Before = render::BufferState::HostWrite,
            .After = render::BufferState::CopySource});
    }
    barriersBefore.emplace_back(render::BarrierTextureDescriptor{
        .Target = request.DstTexture,
        .Before = request.Before,
        .After = render::TextureState::CopyDestination});
    cmdBuffer->ResourceBarrier(barriersBefore);

    // 录制 copy
    cmdBuffer->CopyBufferToTexture(
        request.DstTexture, request.DstRange,
        alloc.Buffer, alloc.Offset);

    // 录制 barrier: dst → ShaderRead
    render::ResourceBarrierDescriptor barrierAfter = render::BarrierTextureDescriptor{
        .Target = request.DstTexture,
        .Before = render::TextureState::CopyDestination,
        .After = request.After};
    cmdBuffer->ResourceBarrier(std::span{&barrierAfter, 1});
}

void ResourceUploader::EndFlight(uint32_t flightIndex) {
    _stagingPool.RetireToFlight(flightIndex);
}

void ResourceUploader::CollectFlight(uint32_t flightIndex) {
    _stagingPool.CollectFlight(flightIndex);
}

std::optional<render::RenderMesh> ResourceUploader::UploadMeshResource(
    render::CommandBuffer* cmdBuffer,
    const MeshResource& meshResource) {
    if (meshResource.Primitives.empty()) {
        return std::nullopt;
    }

    render::RenderMesh result;
    vector<Nullable<render::Buffer*>> bufferByBin(meshResource.Bins.size());

    // 为每个 bin 创建 device-local buffer 并上传
    for (size_t binIdx = 0; binIdx < meshResource.Bins.size(); ++binIdx) {
        const MeshBuffer& bin = meshResource.Bins[binIdx];
        auto data = bin.GetData();
        if (data.empty()) {
            continue;
        }

        render::BufferDescriptor bufDesc{
            .Size = data.size(),
            .Memory = render::MemoryType::Device,
            .Usage = render::BufferUse::Vertex | render::BufferUse::Index | render::BufferUse::CopyDestination,
            .Hints = render::ResourceHint::None};
        auto bufOpt = _device->CreateBuffer(bufDesc);
        if (!bufOpt.HasValue()) {
            return std::nullopt;
        }
        auto buf = bufOpt.Release();
        buf->SetDebugName(fmt::format("{}_{}", meshResource.Name, binIdx));

        UploadBuffer(cmdBuffer, BufferUploadRequest{
                                    .SrcData = data,
                                    .DstBuffer = buf.get(),
                                    .DstOffset = 0,
                                    .Before = render::BufferState::Common,
                                    .After = render::BufferState::Vertex | render::BufferState::Index});

        bufferByBin[binIdx] = buf.get();
        result._buffers.emplace_back(std::move(buf));
    }

    // 为每个 primitive 构建 DrawData
    for (size_t primIdx = 0; primIdx < meshResource.Primitives.size(); ++primIdx) {
        const MeshPrimitive& prim = meshResource.Primitives[primIdx];

        render::RenderMesh::DrawData drawData{};

        // 取第一个 vertex buffer entry 构建 VBV
        if (!prim.VertexBuffers.empty()) {
            const VertexBufferEntry& vbEntry = prim.VertexBuffers[0];
            if (vbEntry.BufferIndex < bufferByBin.size() && bufferByBin[vbEntry.BufferIndex].HasValue()) {
                uint64_t vbSize = static_cast<uint64_t>(prim.VertexCount) * vbEntry.Stride;
                drawData.Vbv = render::VertexBufferView{
                    .Target = bufferByBin[vbEntry.BufferIndex].Get(),
                    .Offset = vbEntry.Offset,
                    .Size = vbSize};
            }
        }

        // 构建 IBV
        if (prim.IndexBuffer.BufferIndex < bufferByBin.size() && bufferByBin[prim.IndexBuffer.BufferIndex].HasValue()) {
            drawData.Ibv = render::IndexBufferView{
                .Target = bufferByBin[prim.IndexBuffer.BufferIndex].Get(),
                .Offset = prim.IndexBuffer.Offset,
                .Stride = prim.IndexBuffer.Stride};
        }

        result._drawDatas.emplace_back(drawData);
    }

    return result;
}

// ════════════════════════════════════════════════════════════
//  PSOCache
// ════════════════════════════════════════════════════════════

PSOCache::VertexLayoutKey PSOCache::VertexLayoutKey::From(const render::VertexBufferLayout& layout) {
    VertexLayoutKey key{};
    key.ArrayStride = layout.ArrayStride;
    key.StepMode = layout.StepMode;
    key.Elements.reserve(layout.Elements.size());
    for (const render::VertexElement& element : layout.Elements) {
        key.Elements.emplace_back(VertexElementKey{
            .Offset = element.Offset,
            .Semantic = string{element.Semantic},
            .SemanticIndex = element.SemanticIndex,
            .Format = element.Format,
            .Location = element.Location});
    }
    return key;
}

size_t PSOCache::GraphicsPsoKeyHash::operator()(const GraphicsPsoKey& key) const noexcept {
    HashKeyBuilder hash;
    hash.AddRootSignatureLayoutKey(key.RootLayout);
    hash.AddShaderCompileKey(key.VS);
    hash.AddBool(key.PS.has_value());
    if (key.PS.has_value()) {
        hash.AddShaderCompileKey(key.PS.value());
    }
    hash.AddSize(key.VertexLayouts.size());
    for (const VertexLayoutKey& vertex : key.VertexLayouts) {
        hash.AddVertexLayoutKey(vertex);
    }
    hash.AddPrimitiveState(key.Primitive);
    hash.AddBool(key.DepthStencil.has_value());
    if (key.DepthStencil.has_value()) {
        hash.AddDepthStencilState(key.DepthStencil.value());
    }
    hash.AddMultiSampleState(key.MultiSample);
    hash.AddSize(key.ColorTargets.size());
    for (const render::ColorTargetState& colorTarget : key.ColorTargets) {
        hash.AddColorTargetState(colorTarget);
    }
    return hash.Finish();
}

render::GraphicsPipelineState* PSOCache::GetOrCreate(const GraphicsPsoDesc& desc) {
    if (desc.RootSig == nullptr) {
        RADRAY_ERR_LOG("PSOCache: root signature is null");
        return nullptr;
    }
    if (desc.VS.Target == nullptr) {
        RADRAY_ERR_LOG("PSOCache: vertex shader is null");
        return nullptr;
    }
    if (desc.PS.has_value() && desc.PS->Target == nullptr) {
        RADRAY_ERR_LOG("PSOCache: pixel shader is null");
        return nullptr;
    }

    vector<VertexLayoutKey> vertexLayouts;
    vertexLayouts.reserve(desc.VertexLayouts.size());
    for (const render::VertexBufferLayout& layout : desc.VertexLayouts) {
        vertexLayouts.push_back(VertexLayoutKey::From(layout));
    }

    vector<render::ColorTargetState> colorTargets;
    colorTargets.reserve(desc.ColorTargets.size());
    for (const render::ColorTargetState& colorTarget : desc.ColorTargets) {
        colorTargets.push_back(colorTarget);
    }

    GraphicsPsoKey key{
        .RootLayout = desc.RootLayout,
        .VS = desc.VS.Key,
        .PS = desc.PS.has_value() ? std::optional<ShaderCompileKey>{desc.PS->Key} : std::nullopt,
        .VertexLayouts = std::move(vertexLayouts),
        .Primitive = desc.Primitive,
        .DepthStencil = desc.DepthStencil,
        .MultiSample = desc.MultiSample,
        .ColorTargets = std::move(colorTargets)};
    if (auto it = _cache.find(key); it != _cache.end()) {
        return it->second.get();
    }

    render::GraphicsPipelineStateDescriptor psoDesc{
        desc.RootSig,
        render::ShaderEntry{desc.VS.Target, desc.VS.Key.EntryPoint},
        !desc.PS.has_value()
            ? std::optional<render::ShaderEntry>{}
            : std::optional<render::ShaderEntry>{render::ShaderEntry{desc.PS->Target, desc.PS->Key.EntryPoint}},
        desc.VertexLayouts,
        desc.Primitive,
        desc.DepthStencil,
        desc.MultiSample,
        desc.ColorTargets};

    auto psoOpt = _device->CreateGraphicsPipelineState(psoDesc);
    if (!psoOpt.HasValue()) {
        RADRAY_ERR_LOG("PSOCache: failed to create graphics pipeline state");
        return nullptr;
    }
    render::GraphicsPipelineState* raw = psoOpt.Get();
    _cache.emplace(std::move(key), psoOpt.Release());
    return raw;
}

}  // namespace radray
