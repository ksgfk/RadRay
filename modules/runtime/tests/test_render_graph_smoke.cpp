#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <gtest/gtest.h>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_graph.h>
#include <radray/window/native_window.h>

using namespace radray;
using namespace radray::render;

namespace {

constexpr uint32_t kInitialWidth = 640;
constexpr uint32_t kInitialHeight = 360;
constexpr uint32_t kBackBufferCount = 3;
constexpr uint32_t kFlightFrameCount = 2;
constexpr uint32_t kFrameLoopCount = 4;
constexpr SubresourceRange kSingleSubresource{0, 1, 0, 1};
constexpr std::array<TextureFormat, 2> kFallbackFormats = {
    TextureFormat::BGRA8_UNORM,
    TextureFormat::RGBA8_UNORM,
};
constexpr std::array<byte, 4> kExpectedClearPixel = {
    byte{0xff},
    byte{0x00},
    byte{0x00},
    byte{0xff},
};
constexpr std::array<byte, 4> kExpectedGreenPixel = {
    byte{0x00},
    byte{0xff},
    byte{0x00},
    byte{0xff},
};
constexpr ColorClearValue kClearColor = {{1.0f, 0.0f, 0.0f, 1.0f}};
constexpr ColorClearValue kGreenClearColor = {{0.0f, 1.0f, 0.0f, 1.0f}};

class LogCollector {
public:
    static void Callback(LogLevel level, std::string_view message, void* userData) {
        auto* self = static_cast<LogCollector*>(userData);
        if (self == nullptr) {
            return;
        }
        if (level != LogLevel::Err && level != LogLevel::Critical) {
            return;
        }
        std::lock_guard<std::mutex> lock(self->_mutex);
        self->_errors.emplace_back(message);
    }

    vector<string> GetErrors() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _errors;
    }

    size_t GetErrorCount() const noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        return _errors.size();
    }

    void Truncate(size_t count) noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_errors.size() > count) {
            _errors.resize(count);
        }
    }

private:
    mutable std::mutex _mutex;
    vector<string> _errors;
};

class ScopedGlobalLogCallback {
public:
    explicit ScopedGlobalLogCallback(LogCollector* logs) noexcept {
        SetLogCallback(&LogCollector::Callback, logs);
    }

    ~ScopedGlobalLogCallback() noexcept {
        ClearLogCallback();
    }

    ScopedGlobalLogCallback(const ScopedGlobalLogCallback&) = delete;
    ScopedGlobalLogCallback& operator=(const ScopedGlobalLogCallback&) = delete;
};

struct SurfaceState {
    unique_ptr<NativeWindow> Window{};
    unique_ptr<GpuSurface> Surface{};
    TextureFormat Format{TextureFormat::UNKNOWN};
    vector<TextureState> BackBufferStates{};
    unordered_set<uint32_t> SeenBackBufferIndices{};
};

string JoinErrors(const vector<string>& errors, size_t maxCount = 8) {
    if (errors.empty()) {
        return {};
    }
    const size_t count = std::min(maxCount, errors.size());
    string result = fmt::format("{}", fmt::join(errors.begin(), errors.begin() + count, "\n"));
    if (errors.size() > count) {
        result += fmt::format("\n...({} more)", errors.size() - count);
    }
    return result;
}

std::string_view BackendTestName(RenderBackend backend) noexcept {
    switch (backend) {
        case RenderBackend::D3D12: return "D3D12";
        case RenderBackend::Vulkan: return "Vulkan";
        default: return "Unknown";
    }
}

vector<RenderBackend> GetEnabledRuntimeBackends() noexcept {
    vector<RenderBackend> backends{};
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.emplace_back(RenderBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    backends.emplace_back(RenderBackend::Vulkan);
#endif
    return backends;
}

Nullable<unique_ptr<NativeWindow>> CreateTestWindow(uint32_t width, uint32_t height) noexcept {
#if defined(_WIN32)
    Win32WindowCreateDescriptor desc{};
    desc.Title = "RenderGraphSmoke";
    desc.Width = static_cast<int32_t>(width);
    desc.Height = static_cast<int32_t>(height);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return CreateNativeWindow(desc);
#elif defined(__APPLE__)
    CocoaWindowCreateDescriptor desc{};
    desc.Title = "RenderGraphSmoke";
    desc.Width = static_cast<int32_t>(width);
    desc.Height = static_cast<int32_t>(height);
    desc.X = 120;
    desc.Y = 120;
    desc.Resizable = true;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return CreateNativeWindow(desc);
#else
    RADRAY_UNUSED(width);
    RADRAY_UNUSED(height);
    return nullptr;
#endif
}

bool CreateRuntimeForBackend(
    RenderBackend backend,
    LogCollector* logs,
    unique_ptr<GpuRuntime>& runtime,
    string& reason) {
    switch (backend) {
        case RenderBackend::D3D12: {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
            D3D12DeviceDescriptor desc{};
            desc.AdapterIndex = std::nullopt;
            desc.IsEnableDebugLayer = true;
            desc.IsEnableGpuBasedValid = true;
            desc.LogCallback = &LogCollector::Callback;
            desc.LogUserData = logs;
            auto runtimeOpt = GpuRuntime::Create(desc);
            if (!runtimeOpt.HasValue()) {
                reason = "GpuRuntime::Create(D3D12) failed";
                return false;
            }
            runtime = runtimeOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            reason = "D3D12 backend is not enabled for this build";
            return false;
#endif
        }
        case RenderBackend::Vulkan: {
#if defined(RADRAY_ENABLE_VULKAN)
            VulkanInstanceDescriptor instanceDesc{};
            instanceDesc.AppName = "RenderGraphSmoke";
            instanceDesc.AppVersion = 1;
            instanceDesc.EngineName = "RadRay";
            instanceDesc.EngineVersion = 1;
            instanceDesc.IsEnableDebugLayer = true;
            instanceDesc.IsEnableGpuBasedValid = false;
            instanceDesc.LogCallback = &LogCollector::Callback;
            instanceDesc.LogUserData = logs;

            VulkanCommandQueueDescriptor queueDesc{};
            queueDesc.Type = QueueType::Direct;
            queueDesc.Count = 1;

            VulkanDeviceDescriptor deviceDesc{};
            deviceDesc.PhysicalDeviceIndex = std::nullopt;
            deviceDesc.Queues = std::span{&queueDesc, 1};

            auto runtimeOpt = GpuRuntime::Create(deviceDesc, instanceDesc);
            if (!runtimeOpt.HasValue()) {
                reason = "GpuRuntime::Create(Vulkan) failed";
                return false;
            }
            runtime = runtimeOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            reason = "Vulkan backend is not enabled for this build";
            return false;
#endif
        }
        default: {
            reason = "Unsupported backend";
            return false;
        }
    }
}

bool CreateSurfaceForWindow(
    GpuRuntime& runtime,
    SurfaceState& state,
    LogCollector* logs,
    uint32_t width,
    uint32_t height,
    string& reason) {
    auto windowOpt = CreateTestWindow(width, height);
    if (!windowOpt.HasValue()) {
        reason = "Cannot create native window for this platform.";
        return false;
    }

    state.Window = windowOpt.Release();
    const auto nativeHandler = state.Window->GetNativeHandler();
    if (nativeHandler.Handle == nullptr) {
        reason = "Window native handle is null.";
        return false;
    }

    const auto size = state.Window->GetSize();
    const uint32_t surfaceWidth = size.X > 0 ? static_cast<uint32_t>(size.X) : width;
    const uint32_t surfaceHeight = size.Y > 0 ? static_cast<uint32_t>(size.Y) : height;
    if (surfaceWidth == 0 || surfaceHeight == 0) {
        reason = "Window size is invalid before surface creation.";
        return false;
    }

    string lastFailure = "GpuRuntime::CreateSurface failed";
    for (size_t i = 0; i < kFallbackFormats.size(); ++i) {
        const auto format = kFallbackFormats[i];
        const size_t errorBaseline = logs != nullptr ? logs->GetErrorCount() : 0;
        try {
            GpuSurfaceDescriptor surfaceDesc{};
            surfaceDesc.NativeHandler = nativeHandler.Handle;
            surfaceDesc.Width = surfaceWidth;
            surfaceDesc.Height = surfaceHeight;
            surfaceDesc.BackBufferCount = kBackBufferCount;
            surfaceDesc.FlightFrameCount = kFlightFrameCount;
            surfaceDesc.Format = format;
            surfaceDesc.PresentMode = PresentMode::FIFO;
            auto surface = runtime.CreateSurface(surfaceDesc);
            if (surface != nullptr && surface->IsValid()) {
                const uint32_t actualBackBufferCount = surface->_swapchain->GetBackBufferCount();
                if (actualBackBufferCount == 0) {
                    lastFailure = "Surface back buffer count is zero.";
                } else {
                    state.Surface = std::move(surface);
                    state.Format = format;
                    state.BackBufferStates.assign(actualBackBufferCount, TextureState::Undefined);
                    state.SeenBackBufferIndices.clear();
                    return true;
                }
            } else {
                lastFailure = fmt::format("Created invalid surface for format {}", format);
            }
        } catch (const std::exception& ex) {
            lastFailure = fmt::format("CreateSurface failed for format {}: {}", format, ex.what());
        } catch (...) {
            lastFailure = fmt::format("CreateSurface failed for format {} with unknown exception", format);
        }

        if (logs != nullptr && i + 1 < kFallbackFormats.size()) {
            logs->Truncate(errorBaseline);
        }
    }

    reason = lastFailure;
    return false;
}

void DestroySurfaceState(SurfaceState& state) noexcept {
    state.Surface.reset();
    if (state.Window != nullptr) {
        state.Window->Destroy();
    }
    state.Window.reset();
    state.BackBufferStates.clear();
    state.SeenBackBufferIndices.clear();
    state.Format = TextureFormat::UNKNOWN;
}

bool PumpWindowEvents(SurfaceState& state, string& reason) {
    if (state.Window == nullptr) {
        reason = "Window is null.";
        return false;
    }
    state.Window->DispatchEvents();
    if (state.Window->ShouldClose()) {
        reason = "Window closed during smoke test.";
        return false;
    }
    return true;
}

bool ForceSyncAndDrain(GpuRuntime& runtime, GpuSurface* surface, string& reason) {
    if (surface == nullptr || surface->_swapchain == nullptr) {
        reason = "Surface or swapchain is null during force sync.";
        return false;
    }

    auto* queue = surface->_swapchain->GetDesc().PresentQueue;
    if (queue == nullptr) {
        reason = "Present queue is null during force sync.";
        return false;
    }

    try {
        runtime.Wait(queue->GetQueueType(), surface->_queueSlot);
    } catch (const std::exception& ex) {
        reason = fmt::format("GpuRuntime::Wait threw: {}", ex.what());
        return false;
    } catch (...) {
        reason = "GpuRuntime::Wait threw an unknown exception.";
        return false;
    }

    if (!runtime._pendings.empty()) {
        reason = fmt::format(
            "GpuRuntime still has {} pending submissions after GpuRuntime::Wait.",
            runtime._pendings.size());
        return false;
    }
    return true;
}

bool ExpectNoCapturedErrors(const LogCollector& logs, string& reason) {
    const auto errors = logs.GetErrors();
    if (!errors.empty()) {
        reason = fmt::format("Captured render errors:\n{}", JoinErrors(errors));
        return false;
    }
    return true;
}

bool WriteUploadBuffer(GpuBufferHandle handle, std::span<const byte> bytes, string& reason) {
    auto* buffer = static_cast<Buffer*>(handle.NativeHandle);
    if (buffer == nullptr) {
        reason = "Upload buffer native handle is null.";
        return false;
    }

    void* mapped = buffer->Map(0, bytes.size_bytes());
    if (mapped == nullptr) {
        reason = "Upload buffer map returned null.";
        return false;
    }

    std::memcpy(mapped, bytes.data(), bytes.size_bytes());
    buffer->Unmap(0, bytes.size_bytes());
    return true;
}

bool MapReadbackBytes(GpuBufferHandle handle, uint64_t size, vector<byte>& outBytes, string& reason) {
    auto* buffer = static_cast<Buffer*>(handle.NativeHandle);
    if (buffer == nullptr) {
        reason = "Readback buffer native handle is null.";
        return false;
    }

    void* mapped = buffer->Map(0, size);
    if (mapped == nullptr) {
        reason = "Readback buffer map returned null.";
        return false;
    }

    outBytes.resize(static_cast<size_t>(size));
    std::memcpy(outBytes.data(), mapped, static_cast<size_t>(size));
    buffer->Unmap(0, size);
    return true;
}

bool UploadBytesToDeviceBuffer(
    GpuRuntime& runtime,
    GpuBufferHandle dstHandle,
    std::span<const byte> bytes,
    BufferState finalState,
    string& reason) {
    auto* dst = static_cast<Buffer*>(dstHandle.NativeHandle);
    if (dst == nullptr) {
        reason = "Device buffer native handle is null.";
        return false;
    }

    const auto uploadHandle = runtime.CreateBuffer(BufferDescriptor{
        .Size = bytes.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
    });
    if (!uploadHandle.IsValid()) {
        reason = "Failed to create upload buffer.";
        return false;
    }
    if (!WriteUploadBuffer(uploadHandle, bytes, reason)) {
        return false;
    }

    auto context = runtime.BeginAsync(QueueType::Direct);
    if (context == nullptr) {
        reason = "GpuRuntime::BeginAsync returned null.";
        return false;
    }

    auto* upload = static_cast<Buffer*>(uploadHandle.NativeHandle);
    auto* cmd = context->CreateCommandBuffer();
    cmd->Begin();

    vector<ResourceBarrierDescriptor> preCopy{};
    if (runtime.GetDevice()->GetBackend() == RenderBackend::Vulkan) {
        preCopy.emplace_back(BarrierBufferDescriptor{
            .Target = upload,
            .Before = BufferState::HostWrite,
            .After = BufferState::CopySource,
        });
    }
    preCopy.emplace_back(BarrierBufferDescriptor{
        .Target = dst,
        .Before = BufferState::Common,
        .After = BufferState::CopyDestination,
    });
    cmd->ResourceBarrier(preCopy);
    cmd->CopyBufferToBuffer(dst, 0, upload, 0, bytes.size());

    if (finalState != BufferState::CopyDestination) {
        ResourceBarrierDescriptor postCopy = BarrierBufferDescriptor{
            .Target = dst,
            .Before = BufferState::CopyDestination,
            .After = finalState,
        };
        cmd->ResourceBarrier(std::span{&postCopy, 1});
    }

    cmd->End();

    auto task = runtime.SubmitAsync(std::move(context));
    if (!task.IsValid()) {
        reason = "GpuRuntime::SubmitAsync returned invalid task.";
        return false;
    }
    task.Wait();
    runtime.ProcessTasks();
    return true;
}

uint64_t ComputeReadbackSizeForTextureCopy(
    const Device& device,
    TextureFormat format,
    uint32_t width,
    uint32_t height) {
    const uint32_t bpp = GetTextureFormatBytesPerPixel(format);
    const uint64_t tightRowBytes = static_cast<uint64_t>(width) * bpp;
    const uint64_t alignment = std::max<uint64_t>(1, device.GetDetail().TextureDataPitchAlignment);
    return Align(tightRowBytes, alignment) * height;
}

bool RecordBackBufferToPresent(
    GpuFrameContext& frameContext,
    TextureState beforeState,
    string& reason) {
    auto* backBuffer = frameContext.GetBackBuffer();
    if (backBuffer == nullptr) {
        reason = "Frame context returned null back buffer.";
        return false;
    }
    if (beforeState == TextureState::Present) {
        return true;
    }

    auto* cmd = frameContext.CreateCommandBuffer();
    if (cmd == nullptr) {
        reason = "CreateCommandBuffer returned null for present transition.";
        return false;
    }

    cmd->Begin();
    ResourceBarrierDescriptor toPresent = BarrierTextureDescriptor{
        .Target = backBuffer,
        .Before = beforeState,
        .After = TextureState::Present,
    };
    cmd->ResourceBarrier(std::span{&toPresent, 1});
    cmd->End();
    return true;
}

class ClearColorRasterPass final : public IRDGRasterPass {
public:
    ClearColorRasterPass(
        RDGBufferHandle seedBuffer,
        RDGTextureHandle target,
        SubresourceRange range,
        ColorClearValue clearValue) noexcept
        : _seedBuffer(seedBuffer),
          _target(target),
          _range(range),
          _clearValue(clearValue) {}

    void Setup(Builder& builder) override {
        builder
            .UseCBuffer(_seedBuffer, ShaderStage::Graphics, BufferRange::AllRange())
            .UseColorAttachment(
                0,
                _target,
                _range,
                LoadAction::Clear,
                StoreAction::Store,
                _clearValue);
    }

    void Execute(GraphicsCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _seedBuffer{};
    RDGTextureHandle _target{};
    SubresourceRange _range{};
    ColorClearValue _clearValue{};
};

class ClearColorDepthRasterPass final : public IRDGRasterPass {
public:
    ClearColorDepthRasterPass(
        RDGBufferHandle seedBuffer,
        RDGTextureHandle colorTarget,
        RDGTextureHandle depthTarget,
        SubresourceRange range,
        ColorClearValue colorClearValue,
        DepthStencilClearValue depthClearValue) noexcept
        : _seedBuffer(seedBuffer),
          _colorTarget(colorTarget),
          _depthTarget(depthTarget),
          _range(range),
          _colorClearValue(colorClearValue),
          _depthClearValue(depthClearValue) {}

    void Setup(Builder& builder) override {
        builder
            .UseCBuffer(_seedBuffer, ShaderStage::Graphics, BufferRange::AllRange())
            .UseColorAttachment(
                0,
                _colorTarget,
                _range,
                LoadAction::Clear,
                StoreAction::Store,
                _colorClearValue)
            .UseDepthStencilAttachment(
                _depthTarget,
                _range,
                LoadAction::Clear,
                StoreAction::Store,
                LoadAction::DontCare,
                StoreAction::Discard,
                _depthClearValue);
    }

    void Execute(GraphicsCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _seedBuffer{};
    RDGTextureHandle _colorTarget{};
    RDGTextureHandle _depthTarget{};
    SubresourceRange _range{};
    ColorClearValue _colorClearValue{};
    DepthStencilClearValue _depthClearValue{};
};

class DualClearColorRasterPass final : public IRDGRasterPass {
public:
    DualClearColorRasterPass(
        RDGBufferHandle seedBuffer,
        RDGTextureHandle firstTarget,
        RDGTextureHandle secondTarget,
        SubresourceRange range,
        ColorClearValue firstClearValue,
        ColorClearValue secondClearValue) noexcept
        : _seedBuffer(seedBuffer),
          _firstTarget(firstTarget),
          _secondTarget(secondTarget),
          _range(range),
          _firstClearValue(firstClearValue),
          _secondClearValue(secondClearValue) {}

    void Setup(Builder& builder) override {
        builder
            .UseCBuffer(_seedBuffer, ShaderStage::Graphics, BufferRange::AllRange())
            .UseColorAttachment(
                0,
                _firstTarget,
                _range,
                LoadAction::Clear,
                StoreAction::Store,
                _firstClearValue)
            .UseColorAttachment(
                1,
                _secondTarget,
                _range,
                LoadAction::Clear,
                StoreAction::Store,
                _secondClearValue);
    }

    void Execute(GraphicsCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _seedBuffer{};
    RDGTextureHandle _firstTarget{};
    RDGTextureHandle _secondTarget{};
    SubresourceRange _range{};
    ColorClearValue _firstClearValue{};
    ColorClearValue _secondClearValue{};
};

class TrackingClearColorRasterPass final : public IRDGRasterPass {
public:
    TrackingClearColorRasterPass(
        RDGBufferHandle seedBuffer,
        RDGTextureHandle target,
        SubresourceRange range,
        ColorClearValue clearValue,
        Nullable<vector<uint32_t>*> executionOrder,
        uint32_t executionToken,
        Nullable<uint32_t*> executeCount) noexcept
        : _seedBuffer(seedBuffer),
          _target(target),
          _range(range),
          _clearValue(clearValue),
          _executionOrder(executionOrder),
          _executionToken(executionToken),
          _executeCount(executeCount) {}

    void Setup(Builder& builder) override {
        builder
            .UseCBuffer(_seedBuffer, ShaderStage::Graphics, BufferRange::AllRange())
            .UseColorAttachment(
                0,
                _target,
                _range,
                LoadAction::Clear,
                StoreAction::Store,
                _clearValue);
    }

    void Execute(GraphicsCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
        if (_executionOrder != nullptr) {
            _executionOrder->emplace_back(_executionToken);
        }
        if (_executeCount != nullptr) {
            ++(*_executeCount);
        }
    }

private:
    RDGBufferHandle _seedBuffer{};
    RDGTextureHandle _target{};
    SubresourceRange _range{};
    ColorClearValue _clearValue{};
    Nullable<vector<uint32_t>*> _executionOrder{nullptr};
    uint32_t _executionToken{0};
    Nullable<uint32_t*> _executeCount{nullptr};
};

class RenderGraphSmokeTest : public ::testing::TestWithParam<RenderBackend> {
protected:
    void SetUp() override {
        _logScope = make_unique<ScopedGlobalLogCallback>(&_logs);
        if (!CreateRuntimeForBackend(GetParam(), &_logs, _runtime, _reason)) {
            _skipLogValidation = true;
            GTEST_SKIP() << _reason;
        }
        _reason.clear();
    }

    void TearDown() override {
        if (_runtime != nullptr && _surfaceState.Surface != nullptr) {
            string drainReason{};
            EXPECT_TRUE(ForceSyncAndDrain(*_runtime, _surfaceState.Surface.get(), drainReason))
                << drainReason;
        }

        DestroySurfaceState(_surfaceState);
        _runtime.reset();

        if (!_skipLogValidation) {
            string errorReason{};
            EXPECT_TRUE(ExpectNoCapturedErrors(_logs, errorReason))
                << errorReason;
        }

        _logScope.reset();
        _reason.clear();
        _skipLogValidation = false;
    }

    void EnsureSurface(uint32_t width = kInitialWidth, uint32_t height = kInitialHeight) {
        if (_surfaceState.Surface != nullptr) {
            return;
        }
        if (!CreateSurfaceForWindow(*_runtime, _surfaceState, &_logs, width, height, _reason)) {
            _skipLogValidation = true;
            GTEST_SKIP() << _reason;
        }
        _reason.clear();
    }

protected:
    LogCollector _logs{};
    unique_ptr<ScopedGlobalLogCallback> _logScope{};
    unique_ptr<GpuRuntime> _runtime{};
    SurfaceState _surfaceState{};
    string _reason{};
    bool _skipLogValidation{false};
};

TEST_P(RenderGraphSmokeTest, AsyncCopyReadbackProducesExpectedBytes) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    vector<byte> expected(64);
    for (size_t i = 0; i < expected.size(); ++i) {
        expected[i] = byte{static_cast<uint8_t>(i * 3 + 1)};
    }

    const auto uploadBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = expected.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
    });
    const auto readbackBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = expected.size(),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });

    ASSERT_TRUE(uploadBuffer.IsValid());
    ASSERT_TRUE(readbackBuffer.IsValid());
    ASSERT_TRUE(WriteUploadBuffer(uploadBuffer, expected, _reason))
        << _reason;

    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto upload = graph.ImportBuffer(
        uploadBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer,
        "upload-buffer");
    const auto deviceBuffer = graph.AddBuffer(
        expected.size(),
        MemoryType::Device,
        BufferUse::CopySource | BufferUse::CopyDestination,
        "device-buffer");
    const auto readback = graph.ImportBuffer(
        readbackBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "readback-buffer");

    RDGCopyPassBuilder uploadPass{};
    uploadPass
        .SetName("upload-copy")
        .CopyBufferToBuffer(deviceBuffer, 0, upload, 0, expected.size());
    uploadPass._buffers.emplace_back(upload, RDGBufferState{RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, allBuffer});
    uploadPass._buffers.emplace_back(deviceBuffer, RDGBufferState{RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer});
    uploadPass.Build(&graph);

    RDGCopyPassBuilder readbackPass{};
    readbackPass
        .SetName("readback-copy")
        .CopyBufferToBuffer(readback, 0, deviceBuffer, 0, expected.size());
    readbackPass._buffers.emplace_back(deviceBuffer, RDGBufferState{RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, allBuffer});
    readbackPass._buffers.emplace_back(readback, RDGBufferState{RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer});
    readbackPass.Build(&graph);

    // Upload/readback heaps on D3D12 are fixed to copy-compatible states, so host mapping stays outside RDG.
    graph.ExportBuffer(readback, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 2u);

    auto context = _runtime->BeginAsync(QueueType::Direct);
    ASSERT_NE(context, nullptr);
    graph.Execute(compiled, context.get());

    auto task = _runtime->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    task.Wait();
    _runtime->ProcessTasks();
    ASSERT_TRUE(_runtime->_pendings.empty())
        << "ProcessTasks should retire the submitted async context after completion.";

    vector<byte> actual{};
    ASSERT_TRUE(MapReadbackBytes(readbackBuffer, expected.size(), actual, _reason))
        << _reason;
    EXPECT_EQ(actual, expected);
}

TEST_P(RenderGraphSmokeTest, AsyncMultiCopyPassWithOffsetsProducesExpectedReadback) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    vector<byte> firstChunk(32);
    vector<byte> secondChunk(32);
    vector<byte> expected(64, byte{0x00});
    for (size_t i = 0; i < firstChunk.size(); ++i) {
        firstChunk[i] = byte{static_cast<uint8_t>(0x10 + i)};
        secondChunk[i] = byte{static_cast<uint8_t>(0xa0 + i)};
    }
    std::copy(firstChunk.begin(), firstChunk.end(), expected.begin());
    std::copy(secondChunk.begin(), secondChunk.end(), expected.begin() + 32);

    const auto uploadBufferA = _runtime->CreateBuffer(BufferDescriptor{
        .Size = firstChunk.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
    });
    const auto uploadBufferB = _runtime->CreateBuffer(BufferDescriptor{
        .Size = secondChunk.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
    });
    const auto readbackBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = expected.size(),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });

    ASSERT_TRUE(uploadBufferA.IsValid());
    ASSERT_TRUE(uploadBufferB.IsValid());
    ASSERT_TRUE(readbackBuffer.IsValid());
    ASSERT_TRUE(WriteUploadBuffer(uploadBufferA, firstChunk, _reason))
        << _reason;
    ASSERT_TRUE(WriteUploadBuffer(uploadBufferB, secondChunk, _reason))
        << _reason;

    const auto allBuffer = BufferRange::AllRange();
    const BufferRange uploadRange{0, firstChunk.size()};
    RenderGraph graph{};
    const auto uploadA = graph.ImportBuffer(
        uploadBufferA,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer,
        "upload-a");
    const auto uploadB = graph.ImportBuffer(
        uploadBufferB,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer,
        "upload-b");
    const auto deviceBuffer = graph.AddBuffer(
        expected.size(),
        MemoryType::Device,
        BufferUse::CopySource | BufferUse::CopyDestination,
        "merged-device-buffer");
    const auto readback = graph.ImportBuffer(
        readbackBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "merged-readback");

    RDGCopyPassBuilder uploadPass{};
    uploadPass
        .SetName("copy-ranges-with-offsets")
        .CopyBufferToBuffer(deviceBuffer, 0, uploadA, 0, firstChunk.size())
        .CopyBufferToBuffer(deviceBuffer, 32, uploadB, 0, secondChunk.size());
    uploadPass._buffers.emplace_back(uploadA, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        uploadRange});
    uploadPass._buffers.emplace_back(uploadB, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        uploadRange});
    uploadPass._buffers.emplace_back(deviceBuffer, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    uploadPass.Build(&graph);

    RDGCopyPassBuilder readbackPass{};
    readbackPass
        .SetName("readback-merged-buffer")
        .CopyBufferToBuffer(readback, 0, deviceBuffer, 0, expected.size());
    readbackPass._buffers.emplace_back(deviceBuffer, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer});
    readbackPass._buffers.emplace_back(readback, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPass.Build(&graph);

    graph.ExportBuffer(readback, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 2u);

    auto context = _runtime->BeginAsync(QueueType::Direct);
    ASSERT_NE(context, nullptr);
    graph.Execute(compiled, context.get());

    auto task = _runtime->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    task.Wait();
    _runtime->ProcessTasks();

    vector<byte> actual{};
    ASSERT_TRUE(MapReadbackBytes(readbackBuffer, expected.size(), actual, _reason))
        << _reason;
    EXPECT_EQ(actual, expected);
}

TEST_P(RenderGraphSmokeTest, AsyncMultiPassSegmentedBufferWritesProduceExpectedReadbacks) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    vector<byte> firstChunk(32);
    vector<byte> secondChunk(32);
    for (size_t i = 0; i < firstChunk.size(); ++i) {
        firstChunk[i] = byte{static_cast<uint8_t>(0x21 + i)};
        secondChunk[i] = byte{static_cast<uint8_t>(0xb1 + i)};
    }

    const auto uploadBufferA = _runtime->CreateBuffer(BufferDescriptor{
        .Size = firstChunk.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
    });
    const auto uploadBufferB = _runtime->CreateBuffer(BufferDescriptor{
        .Size = secondChunk.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
    });
    const auto readbackBufferA = _runtime->CreateBuffer(BufferDescriptor{
        .Size = firstChunk.size(),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    const auto readbackBufferB = _runtime->CreateBuffer(BufferDescriptor{
        .Size = secondChunk.size(),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });

    ASSERT_TRUE(uploadBufferA.IsValid());
    ASSERT_TRUE(uploadBufferB.IsValid());
    ASSERT_TRUE(readbackBufferA.IsValid());
    ASSERT_TRUE(readbackBufferB.IsValid());
    ASSERT_TRUE(WriteUploadBuffer(uploadBufferA, firstChunk, _reason))
        << _reason;
    ASSERT_TRUE(WriteUploadBuffer(uploadBufferB, secondChunk, _reason))
        << _reason;

    const auto allBuffer = BufferRange::AllRange();
    const BufferRange uploadRange{0, firstChunk.size()};
    const BufferRange firstRange{0, firstChunk.size()};
    const BufferRange secondRange{firstChunk.size(), secondChunk.size()};

    RenderGraph graph{};
    const auto uploadA = graph.ImportBuffer(
        uploadBufferA,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer,
        "segment-upload-a");
    const auto uploadB = graph.ImportBuffer(
        uploadBufferB,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer,
        "segment-upload-b");
    const auto deviceBuffer = graph.AddBuffer(
        firstChunk.size() + secondChunk.size(),
        MemoryType::Device,
        BufferUse::CopySource | BufferUse::CopyDestination,
        "segment-device-buffer");
    const auto readbackA = graph.ImportBuffer(
        readbackBufferA,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "segment-readback-a");
    const auto readbackB = graph.ImportBuffer(
        readbackBufferB,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "segment-readback-b");

    RDGCopyPassBuilder writeFirstBuilder{};
    writeFirstBuilder
        .SetName("write-first-segment")
        .CopyBufferToBuffer(deviceBuffer, 0, uploadA, 0, firstChunk.size());
    writeFirstBuilder._buffers.emplace_back(uploadA, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        uploadRange});
    writeFirstBuilder._buffers.emplace_back(deviceBuffer, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        firstRange});
    const auto writeFirstPass = writeFirstBuilder.Build(&graph);

    RDGCopyPassBuilder writeSecondBuilder{};
    writeSecondBuilder
        .SetName("write-second-segment")
        .CopyBufferToBuffer(deviceBuffer, firstChunk.size(), uploadB, 0, secondChunk.size());
    writeSecondBuilder._buffers.emplace_back(uploadB, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        uploadRange});
    writeSecondBuilder._buffers.emplace_back(deviceBuffer, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        secondRange});
    const auto writeSecondPass = writeSecondBuilder.Build(&graph);

    RDGCopyPassBuilder readFirstBuilder{};
    readFirstBuilder
        .SetName("read-first-segment")
        .CopyBufferToBuffer(readbackA, 0, deviceBuffer, 0, firstChunk.size());
    readFirstBuilder._buffers.emplace_back(deviceBuffer, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        firstRange});
    readFirstBuilder._buffers.emplace_back(readbackA, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    const auto readFirstPass = readFirstBuilder.Build(&graph);

    RDGCopyPassBuilder readSecondBuilder{};
    readSecondBuilder
        .SetName("read-second-segment")
        .CopyBufferToBuffer(readbackB, 0, deviceBuffer, firstChunk.size(), secondChunk.size());
    readSecondBuilder._buffers.emplace_back(deviceBuffer, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        secondRange});
    readSecondBuilder._buffers.emplace_back(readbackB, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    const auto readSecondPass = readSecondBuilder.Build(&graph);

    graph.AddPassDependency(writeFirstPass, writeSecondPass);
    graph.AddPassDependency(writeSecondPass, readFirstPass);
    graph.AddPassDependency(readFirstPass, readSecondPass);
    graph.ExportBuffer(readbackA, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.ExportBuffer(readbackB, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 4u);

    auto context = _runtime->BeginAsync(QueueType::Direct);
    ASSERT_NE(context, nullptr);
    graph.Execute(compiled, context.get());

    auto task = _runtime->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    task.Wait();
    _runtime->ProcessTasks();

    vector<byte> actualFirst{};
    vector<byte> actualSecond{};
    ASSERT_TRUE(MapReadbackBytes(readbackBufferA, firstChunk.size(), actualFirst, _reason))
        << _reason;
    ASSERT_TRUE(MapReadbackBytes(readbackBufferB, secondChunk.size(), actualSecond, _reason))
        << _reason;
    EXPECT_EQ(actualFirst, firstChunk);
    EXPECT_EQ(actualSecond, secondChunk);
}

TEST_P(RenderGraphSmokeTest, AsyncSegmentedWriteBetweenReadsPreservesEarlierRange) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    vector<byte> secondChunk(32);
    vector<byte> initialData(64);
    for (size_t i = 0; i < secondChunk.size(); ++i) {
        secondChunk[i] = byte{static_cast<uint8_t>(0xc0 + i)};
    }
    for (size_t i = 0; i < initialData.size(); ++i) {
        initialData[i] = byte{static_cast<uint8_t>(0x30 + i)};
    }
    vector<byte> expectedFirstHalf(initialData.begin(), initialData.begin() + 32);

    const auto deviceBufferHandle = _runtime->CreateBuffer(BufferDescriptor{
        .Size = initialData.size(),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::CopySource | BufferUse::CopyDestination,
    });
    const auto secondUploadBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = secondChunk.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
    });
    const auto readbackBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = expectedFirstHalf.size(),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });

    ASSERT_TRUE(deviceBufferHandle.IsValid());
    ASSERT_TRUE(secondUploadBuffer.IsValid());
    ASSERT_TRUE(readbackBuffer.IsValid());
    ASSERT_TRUE(UploadBytesToDeviceBuffer(*_runtime, deviceBufferHandle, initialData, BufferState::CopySource, _reason))
        << _reason;
    ASSERT_TRUE(WriteUploadBuffer(secondUploadBuffer, secondChunk, _reason))
        << _reason;

    const auto allBuffer = BufferRange::AllRange();
    const BufferRange partialUploadRange{0, secondChunk.size()};
    const BufferRange firstRange{0, expectedFirstHalf.size()};
    const BufferRange secondRange{expectedFirstHalf.size(), secondChunk.size()};

    RenderGraph graph{};
    const auto deviceBuffer = graph.ImportBuffer(
        deviceBufferHandle,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        firstRange,
        "fallback-device-buffer");
    const auto secondUpload = graph.ImportBuffer(
        secondUploadBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer,
        "fallback-second-upload");
    const auto readback = graph.ImportBuffer(
        readbackBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "fallback-readback");

    RDGCopyPassBuilder lateWriteBuilder{};
    lateWriteBuilder
        .SetName("write-second-half-after-import")
        .CopyBufferToBuffer(deviceBuffer, expectedFirstHalf.size(), secondUpload, 0, secondChunk.size());
    lateWriteBuilder._buffers.emplace_back(secondUpload, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        partialUploadRange});
    lateWriteBuilder._buffers.emplace_back(deviceBuffer, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        secondRange});
    const auto lateWritePass = lateWriteBuilder.Build(&graph);

    RDGCopyPassBuilder readBuilder{};
    readBuilder
        .SetName("read-first-half-after-late-write")
        .CopyBufferToBuffer(readback, 0, deviceBuffer, 0, expectedFirstHalf.size());
    readBuilder._buffers.emplace_back(deviceBuffer, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        firstRange});
    readBuilder._buffers.emplace_back(readback, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    const auto readPass = readBuilder.Build(&graph);

    graph.AddPassDependency(lateWritePass, readPass);
    graph.ExportBuffer(readback, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 2u);

    auto context = _runtime->BeginAsync(QueueType::Direct);
    ASSERT_NE(context, nullptr);
    graph.Execute(compiled, context.get());

    auto task = _runtime->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    task.Wait();
    _runtime->ProcessTasks();

    vector<byte> actual{};
    ASSERT_TRUE(MapReadbackBytes(readbackBuffer, expectedFirstHalf.size(), actual, _reason))
        << _reason;
    EXPECT_EQ(actual, expectedFirstHalf);
}

TEST_P(RenderGraphSmokeTest, AsyncBufferToTextureToReadbackProducesExpectedPixel) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    const uint64_t transferSize = ComputeReadbackSizeForTextureCopy(
        *_runtime->GetDevice(),
        TextureFormat::RGBA8_UNORM,
        1,
        1);
    vector<byte> expectedUpload(static_cast<size_t>(transferSize), byte{0x00});
    std::copy(kExpectedClearPixel.begin(), kExpectedClearPixel.end(), expectedUpload.begin());

    const auto uploadBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = transferSize,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
    });
    const auto readbackBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = transferSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });

    ASSERT_TRUE(uploadBuffer.IsValid());
    ASSERT_TRUE(readbackBuffer.IsValid());
    ASSERT_TRUE(WriteUploadBuffer(uploadBuffer, expectedUpload, _reason))
        << _reason;

    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto upload = graph.ImportBuffer(
        uploadBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer,
        "texture-upload");
    const auto texture = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::CopyDestination | TextureUse::CopySource,
        "staging-texture");
    const auto readback = graph.ImportBuffer(
        readbackBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "texture-readback");

    RDGCopyPassBuilder uploadPass{};
    uploadPass
        .SetName("buffer-to-texture")
        .CopyBufferToTexture(texture, kSingleSubresource, upload, 0);
    uploadPass._buffers.emplace_back(upload, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer});
    uploadPass._textures.emplace_back(texture, RDGTextureState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        RDGTextureLayout::TransferDestination,
        kSingleSubresource});
    uploadPass.Build(&graph);

    RDGCopyPassBuilder readbackPass{};
    readbackPass
        .SetName("texture-to-buffer")
        .CopyTextureToBuffer(readback, 0, texture, kSingleSubresource);
    readbackPass._textures.emplace_back(texture, RDGTextureState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        kSingleSubresource});
    readbackPass._buffers.emplace_back(readback, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPass.Build(&graph);

    graph.ExportBuffer(readback, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 2u);

    auto context = _runtime->BeginAsync(QueueType::Direct);
    ASSERT_NE(context, nullptr);
    graph.Execute(compiled, context.get());

    auto task = _runtime->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    task.Wait();
    _runtime->ProcessTasks();

    vector<byte> actual{};
    ASSERT_TRUE(MapReadbackBytes(readbackBuffer, transferSize, actual, _reason))
        << _reason;
    ASSERT_GE(actual.size(), kExpectedClearPixel.size());
    EXPECT_TRUE(std::equal(kExpectedClearPixel.begin(), kExpectedClearPixel.end(), actual.begin()));
}

TEST_P(RenderGraphSmokeTest, AsyncMultiRenderTargetClearReadbackProducesExpectedPixels) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    vector<byte> seedBytes(256, byte{0x00});
    const uint64_t readbackSize = ComputeReadbackSizeForTextureCopy(
        *_runtime->GetDevice(),
        TextureFormat::RGBA8_UNORM,
        1,
        1);
    const auto seedBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = seedBytes.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CBuffer | BufferUse::MapWrite,
    });
    const auto readbackBufferA = _runtime->CreateBuffer(BufferDescriptor{
        .Size = readbackSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    const auto readbackBufferB = _runtime->CreateBuffer(BufferDescriptor{
        .Size = readbackSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    ASSERT_TRUE(seedBuffer.IsValid());
    ASSERT_TRUE(readbackBufferA.IsValid());
    ASSERT_TRUE(readbackBufferB.IsValid());
    ASSERT_TRUE(WriteUploadBuffer(seedBuffer, seedBytes, _reason))
        << _reason;

    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto seed = graph.ImportBuffer(
        seedBuffer,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ConstantRead,
        allBuffer,
        "mrt-seed");
    const auto colorA = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::CopySource,
        "mrt-color-a");
    const auto colorB = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::CopySource,
        "mrt-color-b");
    const auto readbackA = graph.ImportBuffer(
        readbackBufferA,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "mrt-readback-a");
    const auto readbackB = graph.ImportBuffer(
        readbackBufferB,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "mrt-readback-b");

    graph.AddRasterPass(
        "clear-mrt",
        make_unique<DualClearColorRasterPass>(
            seed,
            colorA,
            colorB,
            kSingleSubresource,
            kClearColor,
            kGreenClearColor));

    RDGCopyPassBuilder readbackPassA{};
    readbackPassA
        .SetName("readback-mrt-a")
        .CopyTextureToBuffer(readbackA, 0, colorA, kSingleSubresource);
    readbackPassA._textures.emplace_back(colorA, RDGTextureState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        kSingleSubresource});
    readbackPassA._buffers.emplace_back(readbackA, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPassA.Build(&graph);

    RDGCopyPassBuilder readbackPassB{};
    readbackPassB
        .SetName("readback-mrt-b")
        .CopyTextureToBuffer(readbackB, 0, colorB, kSingleSubresource);
    readbackPassB._textures.emplace_back(colorB, RDGTextureState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        kSingleSubresource});
    readbackPassB._buffers.emplace_back(readbackB, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPassB.Build(&graph);

    graph.ExportBuffer(readbackA, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.ExportBuffer(readbackB, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 3u);

    auto context = _runtime->BeginAsync(QueueType::Direct);
    ASSERT_NE(context, nullptr);
    graph.Execute(compiled, context.get());

    auto task = _runtime->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    task.Wait();
    _runtime->ProcessTasks();

    vector<byte> actualA{};
    ASSERT_TRUE(MapReadbackBytes(readbackBufferA, readbackSize, actualA, _reason))
        << _reason;
    ASSERT_GE(actualA.size(), kExpectedClearPixel.size());
    EXPECT_TRUE(std::equal(kExpectedClearPixel.begin(), kExpectedClearPixel.end(), actualA.begin()));

    vector<byte> actualB{};
    ASSERT_TRUE(MapReadbackBytes(readbackBufferB, readbackSize, actualB, _reason))
        << _reason;
    ASSERT_GE(actualB.size(), kExpectedGreenPixel.size());
    EXPECT_TRUE(std::equal(kExpectedGreenPixel.begin(), kExpectedGreenPixel.end(), actualB.begin()));
}

TEST_P(RenderGraphSmokeTest, AsyncRasterClearWithDepthAttachmentReadbackProducesExpectedColor) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    vector<byte> seedBytes(256, byte{0x00});
    const uint64_t readbackSize = ComputeReadbackSizeForTextureCopy(
        *_runtime->GetDevice(),
        TextureFormat::RGBA8_UNORM,
        1,
        1);
    const auto seedBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = seedBytes.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CBuffer | BufferUse::MapWrite,
    });
    const auto readbackBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = readbackSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    ASSERT_TRUE(seedBuffer.IsValid());
    ASSERT_TRUE(readbackBuffer.IsValid());
    ASSERT_TRUE(WriteUploadBuffer(seedBuffer, seedBytes, _reason))
        << _reason;

    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto seed = graph.ImportBuffer(
        seedBuffer,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ConstantRead,
        allBuffer,
        "graphics-seed");
    const auto color = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::CopySource,
        "color-target");
    const auto depth = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::D32_FLOAT,
        MemoryType::Device,
        TextureUse::DepthStencilWrite,
        "depth-target");
    const auto readback = graph.ImportBuffer(
        readbackBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "depth-readback");

    graph.AddRasterPass(
        "clear-color-depth",
        make_unique<ClearColorDepthRasterPass>(
            seed,
            color,
            depth,
            kSingleSubresource,
            kClearColor,
            DepthStencilClearValue{1.0f, uint8_t{0}}));

    RDGCopyPassBuilder readbackPass{};
    readbackPass
        .SetName("readback-color")
        .CopyTextureToBuffer(readback, 0, color, kSingleSubresource);
    readbackPass._textures.emplace_back(color, RDGTextureState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        kSingleSubresource});
    readbackPass._buffers.emplace_back(readback, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPass.Build(&graph);

    graph.ExportBuffer(readback, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 2u);

    auto context = _runtime->BeginAsync(QueueType::Direct);
    ASSERT_NE(context, nullptr);
    graph.Execute(compiled, context.get());

    auto task = _runtime->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    task.Wait();
    _runtime->ProcessTasks();

    vector<byte> actual{};
    ASSERT_TRUE(MapReadbackBytes(readbackBuffer, readbackSize, actual, _reason))
        << _reason;
    ASSERT_GE(actual.size(), kExpectedClearPixel.size());
    EXPECT_TRUE(std::equal(kExpectedClearPixel.begin(), kExpectedClearPixel.end(), actual.begin()));
}

TEST_P(RenderGraphSmokeTest, PassDependencyOrdersIndependentRasterPasses) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    vector<byte> seedBytes(256, byte{0x00});
    const uint64_t readbackSize = ComputeReadbackSizeForTextureCopy(
        *_runtime->GetDevice(),
        TextureFormat::RGBA8_UNORM,
        1,
        1);
    const auto seedBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = seedBytes.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CBuffer | BufferUse::MapWrite,
    });
    const auto readbackBufferA = _runtime->CreateBuffer(BufferDescriptor{
        .Size = readbackSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    const auto readbackBufferB = _runtime->CreateBuffer(BufferDescriptor{
        .Size = readbackSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    ASSERT_TRUE(seedBuffer.IsValid());
    ASSERT_TRUE(readbackBufferA.IsValid());
    ASSERT_TRUE(readbackBufferB.IsValid());
    ASSERT_TRUE(WriteUploadBuffer(seedBuffer, seedBytes, _reason))
        << _reason;

    const auto allBuffer = BufferRange::AllRange();
    vector<uint32_t> executionOrder{};
    uint32_t firstExecuteCount = 0;
    uint32_t secondExecuteCount = 0;

    RenderGraph graph{};
    const auto seed = graph.ImportBuffer(
        seedBuffer,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ConstantRead,
        allBuffer,
        "ordered-seed");
    const auto firstColor = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::CopySource,
        "ordered-first-color");
    const auto secondColor = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::CopySource,
        "ordered-second-color");
    const auto readbackA = graph.ImportBuffer(
        readbackBufferA,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "ordered-readback-a");
    const auto readbackB = graph.ImportBuffer(
        readbackBufferB,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "ordered-readback-b");

    const auto firstPass = graph.AddRasterPass(
        "ordered-first-pass",
        make_unique<TrackingClearColorRasterPass>(
            seed,
            firstColor,
            kSingleSubresource,
            kClearColor,
            &executionOrder,
            1,
            &firstExecuteCount));
    const auto secondPass = graph.AddRasterPass(
        "ordered-second-pass",
        make_unique<TrackingClearColorRasterPass>(
            seed,
            secondColor,
            kSingleSubresource,
            kGreenClearColor,
            &executionOrder,
            2,
            &secondExecuteCount));
    graph.AddPassDependency(firstPass, secondPass);

    RDGCopyPassBuilder readbackPassA{};
    readbackPassA
        .SetName("ordered-readback-first")
        .CopyTextureToBuffer(readbackA, 0, firstColor, kSingleSubresource);
    readbackPassA._textures.emplace_back(firstColor, RDGTextureState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        kSingleSubresource});
    readbackPassA._buffers.emplace_back(readbackA, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPassA.Build(&graph);

    RDGCopyPassBuilder readbackPassB{};
    readbackPassB
        .SetName("ordered-readback-second")
        .CopyTextureToBuffer(readbackB, 0, secondColor, kSingleSubresource);
    readbackPassB._textures.emplace_back(secondColor, RDGTextureState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        kSingleSubresource});
    readbackPassB._buffers.emplace_back(readbackB, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPassB.Build(&graph);

    graph.ExportBuffer(readbackA, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.ExportBuffer(readbackB, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 4u);

    auto context = _runtime->BeginAsync(QueueType::Direct);
    ASSERT_NE(context, nullptr);
    graph.Execute(compiled, context.get());

    auto task = _runtime->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    task.Wait();
    _runtime->ProcessTasks();

    EXPECT_EQ(firstExecuteCount, 1u);
    EXPECT_EQ(secondExecuteCount, 1u);
    ASSERT_EQ(executionOrder.size(), 2u);
    EXPECT_EQ(executionOrder[0], 1u);
    EXPECT_EQ(executionOrder[1], 2u);

    vector<byte> actualA{};
    ASSERT_TRUE(MapReadbackBytes(readbackBufferA, readbackSize, actualA, _reason))
        << _reason;
    ASSERT_GE(actualA.size(), kExpectedClearPixel.size());
    EXPECT_TRUE(std::equal(kExpectedClearPixel.begin(), kExpectedClearPixel.end(), actualA.begin()));

    vector<byte> actualB{};
    ASSERT_TRUE(MapReadbackBytes(readbackBufferB, readbackSize, actualB, _reason))
        << _reason;
    ASSERT_GE(actualB.size(), kExpectedGreenPixel.size());
    EXPECT_TRUE(std::equal(kExpectedGreenPixel.begin(), kExpectedGreenPixel.end(), actualB.begin()));
}

TEST_P(RenderGraphSmokeTest, CompiledCopyGraphCanExecuteMultipleTimesWithUpdatedUploadData) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    const auto uploadBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::MapWrite | BufferUse::CopySource,
    });
    const auto readbackBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    ASSERT_TRUE(uploadBuffer.IsValid());
    ASSERT_TRUE(readbackBuffer.IsValid());

    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto upload = graph.ImportBuffer(
        uploadBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer,
        "repeat-upload");
    const auto source = graph.AddBuffer(
        64,
        MemoryType::Device,
        BufferUse::CopyDestination | BufferUse::CopySource,
        "repeat-source");
    const auto readback = graph.ImportBuffer(
        readbackBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "repeat-readback");

    RDGCopyPassBuilder uploadPass{};
    uploadPass
        .SetName("upload-source")
        .CopyBufferToBuffer(source, 0, upload, 0, 64);
    uploadPass._buffers.emplace_back(upload, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer});
    uploadPass._buffers.emplace_back(source, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    uploadPass.Build(&graph);

    RDGCopyPassBuilder readbackPass{};
    readbackPass
        .SetName("readback-source")
        .CopyBufferToBuffer(readback, 0, source, 0, 64);
    readbackPass._buffers.emplace_back(source, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        allBuffer});
    readbackPass._buffers.emplace_back(readback, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPass.Build(&graph);
    graph.ExportBuffer(readback, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 2u);

    vector<vector<byte>> expectedSets{};
    expectedSets.emplace_back(64);
    expectedSets.emplace_back(64);
    for (size_t i = 0; i < expectedSets[0].size(); ++i) {
        expectedSets[0][i] = byte{static_cast<uint8_t>(i + 1)};
        expectedSets[1][i] = byte{static_cast<uint8_t>(255 - i)};
    }

    for (size_t iteration = 0; iteration < expectedSets.size(); ++iteration) {
        SCOPED_TRACE(fmt::format("Iteration={}", iteration));

        ASSERT_TRUE(WriteUploadBuffer(uploadBuffer, expectedSets[iteration], _reason))
            << _reason;

        auto context = _runtime->BeginAsync(QueueType::Direct);
        ASSERT_NE(context, nullptr);
        graph.Execute(compiled, context.get());

        auto task = _runtime->SubmitAsync(std::move(context));
        ASSERT_TRUE(task.IsValid());
        task.Wait();
        _runtime->ProcessTasks();
        ASSERT_TRUE(_runtime->_pendings.empty())
            << "ProcessTasks should retire the submitted async context after completion.";

        vector<byte> actual{};
        ASSERT_TRUE(MapReadbackBytes(readbackBuffer, 64, actual, _reason))
            << _reason;
        EXPECT_EQ(actual, expectedSets[iteration]);
    }
}

TEST_P(RenderGraphSmokeTest, UnexportedRasterBranchIsPrunedBeforeExecution) {
    SCOPED_TRACE(fmt::format("Backend={}", BackendTestName(GetParam())));

    vector<byte> seedBytes(256, byte{0x00});
    const uint64_t readbackSize = ComputeReadbackSizeForTextureCopy(
        *_runtime->GetDevice(),
        TextureFormat::RGBA8_UNORM,
        1,
        1);
    const auto seedBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = seedBytes.size(),
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CBuffer | BufferUse::MapWrite,
    });
    const auto readbackBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = readbackSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    ASSERT_TRUE(seedBuffer.IsValid());
    ASSERT_TRUE(readbackBuffer.IsValid());
    ASSERT_TRUE(WriteUploadBuffer(seedBuffer, seedBytes, _reason))
        << _reason;

    const auto allBuffer = BufferRange::AllRange();
    vector<uint32_t> executionOrder{};
    uint32_t liveExecuteCount = 0;
    uint32_t deadExecuteCount = 0;

    RenderGraph graph{};
    const auto seed = graph.ImportBuffer(
        seedBuffer,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ConstantRead,
        allBuffer,
        "dce-seed");
    const auto liveColor = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::CopySource,
        "dce-live-color");
    const auto deadColor = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::CopySource,
        "dce-dead-color");
    const auto readback = graph.ImportBuffer(
        readbackBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "dce-readback");

    graph.AddRasterPass(
        "live-clear",
        make_unique<TrackingClearColorRasterPass>(
            seed,
            liveColor,
            kSingleSubresource,
            kClearColor,
            &executionOrder,
            1,
            &liveExecuteCount));
    graph.AddRasterPass(
        "dead-clear",
        make_unique<TrackingClearColorRasterPass>(
            seed,
            deadColor,
            kSingleSubresource,
            kGreenClearColor,
            &executionOrder,
            2,
            &deadExecuteCount));

    RDGCopyPassBuilder readbackPass{};
    readbackPass
        .SetName("dce-readback-live")
        .CopyTextureToBuffer(readback, 0, liveColor, kSingleSubresource);
    readbackPass._textures.emplace_back(liveColor, RDGTextureState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        kSingleSubresource});
    readbackPass._buffers.emplace_back(readback, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPass.Build(&graph);

    graph.ExportBuffer(readback, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 2u);
    ASSERT_LT(deadColor.Id, compiled._lifetimes.size());
    EXPECT_FALSE(compiled._lifetimes[deadColor.Id].IsValid());

    auto context = _runtime->BeginAsync(QueueType::Direct);
    ASSERT_NE(context, nullptr);
    graph.Execute(compiled, context.get());

    auto task = _runtime->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    task.Wait();
    _runtime->ProcessTasks();

    EXPECT_EQ(liveExecuteCount, 1u);
    EXPECT_EQ(deadExecuteCount, 0u);
    ASSERT_EQ(executionOrder.size(), 1u);
    EXPECT_EQ(executionOrder.front(), 1u);

    vector<byte> actual{};
    ASSERT_TRUE(MapReadbackBytes(readbackBuffer, readbackSize, actual, _reason))
        << _reason;
    ASSERT_GE(actual.size(), kExpectedClearPixel.size());
    EXPECT_TRUE(std::equal(kExpectedClearPixel.begin(), kExpectedClearPixel.end(), actual.begin()));
}

TEST_P(RenderGraphSmokeTest, FrameLoopRasterClearReadbackAndPresentWorks) {
    EnsureSurface();
    SCOPED_TRACE(fmt::format("Backend={} PresentMode=FIFO", BackendTestName(GetParam())));

    const uint64_t readbackSize = ComputeReadbackSizeForTextureCopy(
        *_runtime->GetDevice(),
        TextureFormat::RGBA8_UNORM,
        1,
        1);
    const auto seedBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = 256,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CBuffer | BufferUse::MapWrite,
    });
    const auto readbackBuffer = _runtime->CreateBuffer(BufferDescriptor{
        .Size = readbackSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    ASSERT_TRUE(seedBuffer.IsValid());
    ASSERT_TRUE(readbackBuffer.IsValid());

    const auto allBuffer = BufferRange::AllRange();
    RenderGraph graph{};
    const auto seed = graph.ImportBuffer(
        seedBuffer,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ConstantRead,
        allBuffer,
        "frame-seed");
    const auto frameColor = graph.AddTexture(
        TextureDimension::Dim2D,
        1,
        1,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::CopySource,
        "frame-color");
    const auto readback = graph.ImportBuffer(
        readbackBuffer,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer,
        "frame-readback");

    graph.AddRasterPass(
        "clear-color",
        make_unique<ClearColorRasterPass>(seed, frameColor, kSingleSubresource, kClearColor));

    RDGCopyPassBuilder readbackPass{};
    readbackPass
        .SetName("texture-readback")
        .CopyTextureToBuffer(readback, 0, frameColor, kSingleSubresource);
    readbackPass._textures.emplace_back(frameColor, RDGTextureState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        kSingleSubresource});
    readbackPass._buffers.emplace_back(readback, RDGBufferState{
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        allBuffer});
    readbackPass.Build(&graph);

    graph.ExportBuffer(readback, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    const auto validate = graph.Validate();
    ASSERT_TRUE(validate.IsValid)
        << validate.Message;

    const auto compiled = graph.Compile();
    ASSERT_EQ(compiled._passes.size(), 2u);

    const vector<byte> expectedPixel(kExpectedClearPixel.begin(), kExpectedClearPixel.end());
    for (uint32_t frameIndex = 0; frameIndex < kFrameLoopCount; ++frameIndex) {
        SCOPED_TRACE(fmt::format("Frame={}", frameIndex));

        ASSERT_TRUE(PumpWindowEvents(_surfaceState, _reason))
            << _reason;

        auto begin = _runtime->BeginFrame(_surfaceState.Surface.get());
        ASSERT_EQ(begin.Status, SwapChainStatus::Success)
            << "BeginFrame failed with status " << static_cast<int32_t>(begin.Status);
        ASSERT_TRUE(begin.Context.HasValue())
            << "BeginFrame returned Success without a frame context.";

        auto frameContext = begin.Context.Release();
        ASSERT_NE(frameContext, nullptr);

        graph.Execute(compiled, frameContext.get());

        const uint32_t backBufferIndex = frameContext->GetBackBufferIndex();
        ASSERT_LT(backBufferIndex, _surfaceState.BackBufferStates.size());
        ASSERT_TRUE(RecordBackBufferToPresent(
            *frameContext,
            _surfaceState.BackBufferStates[backBufferIndex],
            _reason))
            << _reason;

        auto submit = _runtime->SubmitFrame(std::move(frameContext));
        ASSERT_TRUE(submit.Task.IsValid())
            << "SubmitFrame returned an invalid task.";
        ASSERT_EQ(submit.Present.Status, SwapChainStatus::Success)
            << "SubmitFrame present failed with status " << static_cast<int32_t>(submit.Present.Status)
            << " native " << submit.Present.NativeStatusCode;

        _surfaceState.BackBufferStates[backBufferIndex] = TextureState::Present;
        _surfaceState.SeenBackBufferIndices.emplace(backBufferIndex);

        ASSERT_TRUE(ForceSyncAndDrain(*_runtime, _surfaceState.Surface.get(), _reason))
            << _reason;

        vector<byte> actualPixel{};
        ASSERT_TRUE(MapReadbackBytes(readbackBuffer, expectedPixel.size(), actualPixel, _reason))
            << _reason;
        EXPECT_EQ(actualPixel, expectedPixel);
    }

    EXPECT_GE(_surfaceState.SeenBackBufferIndices.size(), 2u)
        << "Swapchain did not rotate back buffers as expected.";
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    RenderGraphSmokeTest,
    ::testing::ValuesIn(GetEnabledRuntimeBackends()),
    [](const ::testing::TestParamInfo<RenderBackend>& info) {
        return string{BackendTestName(info.param)};
    });

}  // namespace
