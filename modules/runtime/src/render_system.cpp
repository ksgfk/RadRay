#include <radray/runtime/render_system.h>

#include <radray/basic_math.h>
#include <radray/file.h>
#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/render/shader_compiler/spvc.h>
#include <radray/vertex_data.h>
#include <radray/runtime/application.h>
#include <radray/runtime/material.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/vertex_factory.h>
#include <radray/runtime/window_system.h>

#include <algorithm>
#include <cstring>

namespace radray {

namespace {

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
//  AppRenderSystem
// ═══════════════════════════════════════════════════════════════

bool AppRenderSystem::CompleteFlight(uint32_t flightIndex) {
    auto& flight = _flights[flightIndex];
    if (!flight.Signal.IsValid() || flight.Signal.Fence->GetCompletedValue() < flight.Signal.Value) {
        return false;
    }

    _lastFrameLatency = std::chrono::steady_clock::now() - flight.FrameStartTime;
    flight.Signal = AppRenderSystem::FenceSignal::Invalid();
    _app->NotifyRenderComplete(AppRenderCompleteContext{.FlightIndex = flightIndex});
    flight.WaitForDestroy.clear();
    _uploader->CollectFlight(flightIndex);
    return true;
}

AppRenderSystem::AppRenderSystem(Application* app, const AppRenderSystemDescriptor& desc)
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
    _psoCache = make_unique<PSOCache>(_device);
}

AppRenderSystem::~AppRenderSystem() noexcept = default;

Nullable<render::Shader*> AppRenderSystem::GetOrCompileShader(const ShaderCompileDescriptor& desc) {
    const bool isSpirv = _device->GetBackend() == render::RenderBackend::Vulkan;
    string key = fmt::format(
        "{}|{}|{}|{}",
        desc.Name,
        desc.EntryPoint,
        static_cast<uint32_t>(desc.Stage),
        isSpirv ? "spirv" : "dxil");
    if (auto it = _shaderCache.find(key); it != _shaderCache.end()) {
        return it->second.get();
    }

    if (_dxc == nullptr) {
        auto dxcOpt = render::CreateDxc();
        if (!dxcOpt.HasValue()) {
            RADRAY_ERR_LOG("AppRenderSystem: failed to create DXC compiler");
            return nullptr;
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
    auto outputOpt = _dxc->Compile(params);
    if (!outputOpt.has_value()) {
        RADRAY_ERR_LOG("AppRenderSystem: failed to compile shader '{}' entry '{}'", desc.Name, desc.EntryPoint);
        return nullptr;
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
            RADRAY_ERR_LOG("AppRenderSystem: failed to reflect SPIR-V shader '{}'", desc.Name);
            return nullptr;
        }
        reflection = std::move(reflOpt.value());
        category = render::ShaderBlobCategory::SPIRV;
#else
        RADRAY_ERR_LOG("AppRenderSystem: SPIR-V Cross reflection is not enabled in this build");
        return nullptr;
#endif
    } else {
        auto reflOpt = _dxc->GetShaderDescFromOutput(output.Refl);
        if (!reflOpt.has_value()) {
            RADRAY_ERR_LOG("AppRenderSystem: failed to reflect DXIL shader '{}'", desc.Name);
            return nullptr;
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
        RADRAY_ERR_LOG("AppRenderSystem: failed to create shader '{}'", desc.Name);
        return nullptr;
    }
    render::Shader* raw = shaderOpt.Get();
    _shaderCache.emplace(std::move(key), shaderOpt.Release());
    return raw;
}

Nullable<render::Shader*> AppRenderSystem::GetOrCompileShaderFromFile(
    const std::filesystem::path& path,
    std::string_view entryPoint,
    render::ShaderStage stage,
    std::string_view name) {
    auto sourceOpt = ReadTextFile(path);
    if (!sourceOpt.has_value()) {
        RADRAY_ERR_LOG("AppRenderSystem: failed to read shader file {}", path.string());
        return nullptr;
    }
    string pathName = name.empty() ? path.string() : string{name};
    ShaderCompileDescriptor desc{};
    desc.Name = pathName;
    desc.Source = sourceOpt.value();
    desc.EntryPoint = entryPoint;
    desc.Stage = stage;
    return GetOrCompileShader(desc);
}

Nullable<render::RootSignature*> AppRenderSystem::GetOrCreateRootSignature(std::span<render::Shader*> shaders) {
    if (shaders.empty()) {
        RADRAY_ERR_LOG("AppRenderSystem: cannot create root signature from empty shader set");
        return nullptr;
    }
    // 按参与的 shader 集合作为身份:同一组 shader 决定同一绑定布局，故共享一个
    // RootSignature。指针来自 shader 缓存(稳定且去重)，排序后拼 key 使顺序无关。
    vector<render::Shader*> sorted{shaders.begin(), shaders.end()};
    std::sort(sorted.begin(), sorted.end());
    string key;
    for (render::Shader* s : sorted) {
        key += fmt::format("{:x}|", reinterpret_cast<uintptr_t>(s));
    }
    if (auto it = _rootSigCache.find(key); it != _rootSigCache.end()) {
        return it->second.get();
    }

    render::RootSignatureDescriptor rsDesc{};
    rsDesc.Shaders = shaders;
    auto rsOpt = _device->CreateRootSignature(rsDesc);
    if (!rsOpt.HasValue()) {
        RADRAY_ERR_LOG("AppRenderSystem: failed to create root signature");
        return nullptr;
    }
    render::RootSignature* raw = rsOpt.Get();
    _rootSigCache.emplace(std::move(key), rsOpt.Release());
    return raw;
}

void AppRenderSystem::WaitAndCleanupCompletedFlights() {
    _mainQueue->Wait();

    for (uint32_t flightIndex = 0; flightIndex < _flights.size(); ++flightIndex) {
        CompleteFlight(flightIndex);
    }
}

AppFrameContext AppRenderSystem::BeginFrameRecord(
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
    return AppFrameContext{this, flightIndex, deltaTime, lastFrameLatency, isInModalLoop};
}

void AppRenderSystem::EndFrameRecordAndSubmit(uint32_t flightIndex) {
    FlightSlot& record = _flights[flightIndex];
    if (!record.Recording) {
        return;
    }
    record.Recording = false;

    // 闭合上传链路：本帧录制的 staging + AssetRef 绑定到该 flight。
    _uploader->EndFlight(flightIndex);

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
    uint64_t signalValues[] = {_mainQueueTrack.NextFenceValue++};
    render::CommandQueueSubmitDescriptor submitDesc{
        .CmdBuffers = std::span{submitCmdBuffers},
        .SignalFences = std::span{signalFences},
        .SignalValues = std::span{signalValues},
        .WaitToExecute = std::span{waitToExecute},
        .ReadyToPresent = std::span{readyToPresent}};
    _mainQueue->Submit(submitDesc);
    _flights[flightIndex].Signal = AppRenderSystem::FenceSignal{
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
//  AppFrameContext
// ══════════════════════════════════════════════

render::CommandBuffer* AppFrameContext::GetCommandBuffer() const noexcept {
    return _renderSystem->_flights[_flightIndex].CmdBuffer.get();
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
    render::TextureView* backBufferView = window->GetOrCreateBackBufferView(frame, _flightIndex);
    if (backBufferView == nullptr) {
        // 未能建立 view：丢弃该 frame（不提交）。
        return std::nullopt;
    }

    AppRenderSystem::FlightSlot& record = _renderSystem->_flights[_flightIndex];
    record.Targets.emplace_back(AppRenderSystem::FlightSlot::AcquiredTarget{
        .Window = window,
        .Frame = std::move(frame)});
    return AppFrameTarget{
        .Window = window,
        .BackBuffer = backBuffer,
        .BackBufferView = backBufferView};
}

ResourceUploader& AppFrameContext::GetUploader() const noexcept {
    return *_renderSystem->_uploader;
}

render::Device* AppFrameContext::GetDevice() const noexcept {
    return _renderSystem->_device;
}

render::CommandQueue* AppFrameContext::GetMainQueue() const noexcept {
    return _renderSystem->_mainQueue;
}

void AppFrameContext::SetManualSubmit() noexcept {
    _renderSystem->_flights[_flightIndex].ManualSubmit = true;
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
    _pendingRefs.resize(flightCount);
}

ResourceUploader::~ResourceUploader() noexcept = default;

void ResourceUploader::UploadBuffer(
    render::CommandBuffer* cmdBuffer,
    const BufferUploadRequest& request,
    AssetRefAny assetRef) {
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

    // 持有强引用
    if (assetRef.IsValid()) {
        _currentRefs.emplace_back(std::move(assetRef));
    }
}

void ResourceUploader::UploadTexture(
    render::CommandBuffer* cmdBuffer,
    const TextureUploadRequest& request,
    AssetRefAny assetRef) {
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

    // 持有强引用
    if (assetRef.IsValid()) {
        _currentRefs.emplace_back(std::move(assetRef));
    }
}

void ResourceUploader::EndFlight(uint32_t flightIndex) {
    _stagingPool.RetireToFlight(flightIndex);
    auto& refs = _pendingRefs[flightIndex];
    refs.insert(refs.end(),
                std::make_move_iterator(_currentRefs.begin()),
                std::make_move_iterator(_currentRefs.end()));
    _currentRefs.clear();
}

void ResourceUploader::CollectFlight(uint32_t flightIndex) {
    _stagingPool.CollectFlight(flightIndex);
    _pendingRefs[flightIndex].clear();
}

std::optional<render::RenderMesh> ResourceUploader::UploadMesh(
    render::CommandBuffer* cmdBuffer,
    StaticMesh* mesh,
    AssetRefAny assetRef) {
    if (mesh == nullptr || !mesh->IsValid()) {
        return std::nullopt;
    }
    const MeshResource& meshResource = mesh->GetMeshResource();
    if (meshResource.Primitives.empty()) {
        return std::nullopt;
    }

    render::RenderMesh result;
    vector<Nullable<render::Buffer*>> bufferByBin(meshResource.Bins.size());

    // 为每个 bin 创建 device-local buffer 并上传
    bool assetRefPassed = false;
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

        // 第一次上传时传递 assetRef，后续不重复传递
        AssetRefAny ref = (!assetRefPassed && assetRef.IsValid())
                              ? AssetRefAny{assetRef}
                              : AssetRefAny{nullptr};
        if (ref.IsValid()) {
            assetRefPassed = true;
        }

        UploadBuffer(cmdBuffer, BufferUploadRequest{
                                    .SrcData = data,
                                    .DstBuffer = buf.get(),
                                    .DstOffset = 0,
                                    .Before = render::BufferState::Common,
                                    .After = render::BufferState::Vertex | render::BufferState::Index},
                     std::move(ref));

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

namespace {

string BuildPSOKey(
    const Material& material,
    const render::VertexBufferLayout& vertexLayout,
    const PSOCache::RenderTargetFormats& rtFormats) {
    // Material 身份用其 AssetId(同一资产同一 PSO 基线);叠加顶点签名与 RT 格式。
    string key = fmt::format("mat={}|vs={}|ps={}|", material.GetAssetId().ToString(), material.GetVsEntry(), material.GetPsEntry());
    key += VertexFactory::BuildSignature(vertexLayout);
    key += "|rt=";
    for (render::TextureFormat f : rtFormats.ColorFormats) {
        key += fmt::format("{},", static_cast<uint32_t>(f));
    }
    key += fmt::format("|ds={}", static_cast<uint32_t>(rtFormats.DepthFormat));
    return key;
}

}  // namespace

render::GraphicsPipelineState* PSOCache::GetOrCreate(
    const Material& material,
    const render::VertexBufferLayout& vertexLayout,
    const RenderTargetFormats& rtFormats) {
    if (!material.IsValid()) {
        RADRAY_ERR_LOG("PSOCache: material is not valid");
        return nullptr;
    }

    string key = BuildPSOKey(material, vertexLayout, rtFormats);
    if (auto it = _cache.find(key); it != _cache.end()) {
        return it->second.get();
    }

    // 渲染状态从 material 取,RT 格式注入到 color/depth 状态里。
    vector<render::ColorTargetState> colorTargets;
    colorTargets.reserve(rtFormats.ColorFormats.size());
    for (render::TextureFormat f : rtFormats.ColorFormats) {
        render::ColorTargetState cts = render::ColorTargetState::Default(f);
        cts.Blend = material.GetBlendState();
        colorTargets.push_back(cts);
    }

    render::DepthStencilState depthState = material.GetDepthStencilState();
    depthState.Format = rtFormats.DepthFormat;

    render::GraphicsPipelineStateDescriptor psoDesc{
        material.GetRootSignature(),
        render::ShaderEntry{material.GetVS(), material.GetVsEntry()},
        render::ShaderEntry{material.GetPS(), material.GetPsEntry()},
        std::span<const render::VertexBufferLayout>{&vertexLayout, 1},
        material.GetPrimitiveState(),
        depthState,
        render::MultiSampleState::Default(),
        std::span<const render::ColorTargetState>{colorTargets.data(), colorTargets.size()}};

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
