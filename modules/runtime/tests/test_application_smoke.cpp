#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <gtest/gtest.h>
#include <fmt/format.h>

#if defined(_WIN32)
extern "C" __declspec(dllimport) int __stdcall PostMessageW(
    void* hWnd,
    unsigned int msg,
    uintptr_t wParam,
    intptr_t lParam);
#endif

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/runtime/application.h>
#include <radray/window/native_window.h>

using namespace radray;
using namespace radray::render;

namespace {

#if defined(_WIN32)
constexpr unsigned int kWin32WmClose = 0x0010;
#endif

constexpr uint32_t kInitialWidth = 640;
constexpr uint32_t kInitialHeight = 360;
constexpr uint32_t kSecondaryWidth = 480;
constexpr uint32_t kSecondaryHeight = 320;
constexpr uint32_t kMaxLogicFrames = 240;

constexpr std::array<ColorClearValue, 3> kClearColors = {{
    {{{1.0f, 0.0f, 0.0f, 1.0f}}},
    {{{0.0f, 1.0f, 0.0f, 1.0f}}},
    {{{0.0f, 0.0f, 1.0f, 1.0f}}},
}};

bool ContainsErrorMessage(const std::vector<std::string>& messages, std::string_view needle) {
    return std::find_if(
               messages.begin(),
               messages.end(),
               [needle](const std::string& message) { return message.find(needle) != std::string::npos; })
           != messages.end();
}

class LogCollector {
public:
    static void Callback(LogLevel level, std::string_view message, void* userData) {
        auto* self = static_cast<LogCollector*>(userData);
        if (self == nullptr || (level != LogLevel::Err && level != LogLevel::Critical)) {
            return;
        }
        std::lock_guard<std::mutex> lock(self->_mutex);
        self->_errors.emplace_back(message);
    }

    std::vector<std::string> GetErrors() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _errors;
    }

private:
    mutable std::mutex _mutex;
    std::vector<std::string> _errors;
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

struct RGBPacket : public FramePacket {
    uint32_t ColorIndex{0};
};

struct AppTestParam {
    RenderBackend Backend{RenderBackend::D3D12};
    bool MultiThreaded{false};
};

class ColorClearCallbacksBase : public IAppCallbacks {
public:
    void OnShutdown(Application* app) override {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& [surfaceId, state] : _surfaceStates) {
            state.CachedViews.clear();
            state.BackBufferStates.clear();
        }
    }

    void OnSurfaceAdded(Application* app, AppSurfaceId surfaceId, GpuSurface* surface) override {
        std::lock_guard<std::mutex> lock(_mutex);
        _surfaceStates[surfaceId].AddedCount++;
    }

    void OnSurfaceRemoved(Application* app, AppSurfaceId surfaceId, NativeWindow* window) override {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& state = _surfaceStates[surfaceId];
        state.RemovedCount++;
        state.CachedViews.clear();
        state.BackBufferStates.clear();
    }

    void OnSurfaceRecreated(Application* app, AppSurfaceId surfaceId, GpuSurface* surface) override {
        std::lock_guard<std::mutex> lock(_mutex);
        auto& state = _surfaceStates[surfaceId];
        state.RecreateCount++;
        state.CachedViews.clear();
        state.BackBufferStates.clear();
    }

    void OnRender(
        Application* app,
        AppSurfaceId surfaceId,
        GpuFrameContext* frameCtx,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo,
        FramePacket* packet) override {
        const auto* rgbPacket = static_cast<RGBPacket*>(packet);
        const uint32_t colorIndex = rgbPacket != nullptr ? rgbPacket->ColorIndex % kClearColors.size() : 0;

        std::lock_guard<std::mutex> lock(_mutex);
        auto& state = _surfaceStates[surfaceId];
        RenderSolidColor(app, surfaceId, frameCtx, state, colorIndex);
        state.FramesRendered++;
        state.LastRenderFrameIndex = surfaceInfo.RenderFrameIndex;
        state.LastDroppedFrameCount = surfaceInfo.DroppedFrameCount;
    }

    bool HasFailures() const {
        std::lock_guard<std::mutex> lock(_failureMutex);
        return !_failures.empty();
    }

    std::vector<std::string> GetFailures() const {
        std::lock_guard<std::mutex> lock(_failureMutex);
        return _failures;
    }

    uint32_t GetRenderCount(AppSurfaceId surfaceId) const {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _surfaceStates.find(surfaceId);
        return it != _surfaceStates.end() ? it->second.FramesRendered : 0;
    }

    uint32_t GetAddedCount(AppSurfaceId surfaceId) const {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _surfaceStates.find(surfaceId);
        return it != _surfaceStates.end() ? it->second.AddedCount : 0;
    }

    uint32_t GetRemovedCount(AppSurfaceId surfaceId) const {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _surfaceStates.find(surfaceId);
        return it != _surfaceStates.end() ? it->second.RemovedCount : 0;
    }

    uint32_t GetRecreateCount(AppSurfaceId surfaceId) const {
        std::lock_guard<std::mutex> lock(_mutex);
        const auto it = _surfaceStates.find(surfaceId);
        return it != _surfaceStates.end() ? it->second.RecreateCount : 0;
    }

protected:
    struct SurfaceRuntimeState {
        std::vector<TextureStates> BackBufferStates;
        std::vector<unique_ptr<TextureView>> CachedViews;
        uint32_t FramesRendered{0};
        uint32_t AddedCount{0};
        uint32_t RemovedCount{0};
        uint32_t RecreateCount{0};
        uint64_t LastRenderFrameIndex{0};
        uint64_t LastDroppedFrameCount{0};
    };

    void RecordFailure(std::string message) {
        std::lock_guard<std::mutex> lock(_failureMutex);
        _failures.emplace_back(std::move(message));
    }

private:
    void RenderSolidColor(
        Application* app,
        AppSurfaceId surfaceId,
        GpuFrameContext* frameCtx,
        SurfaceRuntimeState& state,
        uint32_t colorIndex) {
        auto* surface = app->GetSurface(surfaceId);
        if (surface == nullptr) {
            RecordFailure(fmt::format("surface {} is not available during render", surfaceId));
            return;
        }

        auto* backBuffer = frameCtx->GetBackBuffer();
        const uint32_t backBufferIndex = frameCtx->GetBackBufferIndex();
        auto* device = app->GetGpuRuntime()->GetDevice();

        if (backBufferIndex >= state.CachedViews.size()) {
            state.CachedViews.resize(backBufferIndex + 1);
        }
        if (backBufferIndex >= state.BackBufferStates.size()) {
            state.BackBufferStates.resize(backBufferIndex + 1, TextureState::UNKNOWN);
        }

        if (!state.CachedViews[backBufferIndex]) {
            TextureViewDescriptor viewDesc{};
            viewDesc.Target = backBuffer;
            viewDesc.Dim = TextureDimension::Dim2D;
            viewDesc.Format = surface->GetFormat();
            viewDesc.Range = SubresourceRange{0, 1, 0, 1};
            viewDesc.Usage = TextureViewUsage::RenderTarget;
            auto viewOpt = device->CreateTextureView(viewDesc);
            if (!viewOpt.HasValue()) {
                RecordFailure(fmt::format("CreateTextureView failed for surface {}", surfaceId));
                return;
            }
            state.CachedViews[backBufferIndex] = viewOpt.Release();
        }

        auto* cmd = frameCtx->CreateCommandBuffer();
        auto* view = state.CachedViews[backBufferIndex].get();
        cmd->Begin();

        TextureStates beforeState = TextureState::Undefined;
        if (state.BackBufferStates[backBufferIndex] != TextureStates{TextureState::UNKNOWN}) {
            beforeState = state.BackBufferStates[backBufferIndex];
        }
        if (beforeState != TextureState::RenderTarget) {
            ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{
                backBuffer,
                beforeState,
                TextureState::RenderTarget};
            cmd->ResourceBarrier(std::span{&barrier, 1});
        }

        ColorAttachment colorAttachment{
            view,
            LoadAction::Clear,
            StoreAction::Store,
            kClearColors[colorIndex]};
        RenderPassDescriptor passDesc{};
        passDesc.ColorAttachments = std::span{&colorAttachment, 1};
        auto passOpt = cmd->BeginRenderPass(passDesc);
        if (!passOpt.HasValue()) {
            RecordFailure(fmt::format("BeginRenderPass failed for surface {}", surfaceId));
            cmd->End();
            return;
        }
        cmd->EndRenderPass(passOpt.Release());

        ResourceBarrierDescriptor toPresent = BarrierTextureDescriptor{
            backBuffer,
            TextureState::RenderTarget,
            TextureState::Present};
        cmd->ResourceBarrier(std::span{&toPresent, 1});
        cmd->End();

        state.BackBufferStates[backBufferIndex] = TextureState::Present;
    }

    mutable std::mutex _mutex;
    mutable std::mutex _failureMutex;
    unordered_map<AppSurfaceId, SurfaceRuntimeState> _surfaceStates;
    std::vector<std::string> _failures;
};

class SingleSurfaceCallbacks : public ColorClearCallbacksBase {
public:
    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 1) {
            RecordFailure(fmt::format("expected 1 initial surface, got {}", ids.size()));
            return;
        }
        _primaryId = app->GetPrimarySurfaceId();
    }

    void OnUpdate(Application* app, float dt) override {
        if (HasFailures()) {
            app->RequestExit();
            return;
        }
        if (app->GetFrameInfo().LogicFrameIndex >= 24) {
            app->RequestExit();
            return;
        }
        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("single-surface test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = static_cast<uint32_t>((appInfo.LogicFrameIndex + surfaceId.Handle) % kClearColors.size());
        return packet;
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }

private:
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
};

class MultiInFlightCallbacks : public ColorClearCallbacksBase {
public:
    MultiInFlightCallbacks(uint32_t flightFrameCount, bool expectMultiThreaded)
        : _flightFrameCount(flightFrameCount),
          _expectMultiThreaded(expectMultiThreaded) {}

    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 1) {
            RecordFailure(fmt::format("expected 1 initial surface, got {}", ids.size()));
            return;
        }
        _primaryId = ids.front();
    }

    void OnUpdate(Application* app, float dt) override {
        (void)dt;
        if (HasFailures()) {
            app->RequestExit();
            return;
        }

        if (_expectMultiThreaded) {
            if (GetMaxOutstanding() >= _flightFrameCount && GetRenderCount(_primaryId) >= _flightFrameCount) {
                app->RequestExit();
                return;
            }
        } else if (GetRenderCount(_primaryId) >= _flightFrameCount) {
            app->RequestExit();
            return;
        }

        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("multi-in-flight test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        (void)app;
        (void)appInfo;
        (void)surfaceInfo;

        const uint32_t extracted = _extractedCount.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint32_t rendered = _renderedCount.load(std::memory_order_relaxed);
        UpdateMaxOutstanding(extracted - rendered);

        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = static_cast<uint32_t>(surfaceId.Handle % kClearColors.size());
        return packet;
    }

    void OnRender(
        Application* app,
        AppSurfaceId surfaceId,
        GpuFrameContext* frameCtx,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo,
        FramePacket* packet) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(12));
        ColorClearCallbacksBase::OnRender(app, surfaceId, frameCtx, appInfo, surfaceInfo, packet);
        _renderedCount.fetch_add(1, std::memory_order_relaxed);
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }

    uint32_t GetMaxOutstanding() const {
        return _maxOutstandingCount.load(std::memory_order_relaxed);
    }

private:
    void UpdateMaxOutstanding(uint32_t value) {
        uint32_t current = _maxOutstandingCount.load(std::memory_order_relaxed);
        while (current < value &&
               !_maxOutstandingCount.compare_exchange_weak(current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    uint32_t _flightFrameCount{0};
    bool _expectMultiThreaded{false};
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
    std::atomic<uint32_t> _extractedCount{0};
    std::atomic<uint32_t> _renderedCount{0};
    std::atomic<uint32_t> _maxOutstandingCount{0};
};

class MultiSurfaceCallbacks : public ColorClearCallbacksBase {
public:
    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 2) {
            RecordFailure(fmt::format("expected 2 initial surfaces, got {}", ids.size()));
            return;
        }
        _primaryId = app->GetPrimarySurfaceId();
        _secondaryId = ids[0] == _primaryId ? ids[1] : ids[0];
    }

    void OnUpdate(Application* app, float dt) override {
        if (HasFailures()) {
            app->RequestExit();
            return;
        }

        if (GetRenderCount(_primaryId) >= 6 && GetRenderCount(_secondaryId) >= 6) {
            app->RequestExit();
            return;
        }

        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("multi-surface round-robin test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = surfaceId == _primaryId ? 0u : 1u;
        return packet;
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }
    AppSurfaceId GetSecondaryId() const { return _secondaryId; }

private:
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
    AppSurfaceId _secondaryId{AppSurfaceId::Invalid()};
};

class RuntimeAddRemoveCallbacks : public ColorClearCallbacksBase {
public:
    explicit RuntimeAddRemoveCallbacks(NativeWindow* secondaryWindow)
        : _secondaryWindow(secondaryWindow) {}

    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 1) {
            RecordFailure(fmt::format("expected 1 initial surface, got {}", ids.size()));
            return;
        }
        _primaryId = ids.front();
    }

    void OnUpdate(Application* app, float dt) override {
        if (HasFailures()) {
            app->RequestExit();
            return;
        }

        if (_secondaryId == AppSurfaceId::Invalid() && app->GetFrameInfo().LogicFrameIndex >= 5) {
            AppSurfaceConfig config{};
            config.Window = _secondaryWindow;
            config.SurfaceFormat = TextureFormat::BGRA8_UNORM;
            config.PresentMode = PresentMode::FIFO;
            config.BackBufferCount = 3;
            config.FlightFrameCount = 2;
            _secondaryId = app->RequestAddSurface(config);
            if (_secondaryId == AppSurfaceId::Invalid()) {
                RecordFailure("RequestAddSurface returned an invalid id");
                app->RequestExit();
            }
        }

        if (_secondaryAdded && !_removeRequested && GetRenderCount(_secondaryId) >= 4) {
            _removeRequested = true;
            app->RequestRemoveSurface(_secondaryId);
        }

        if (_secondaryRemoved && GetRenderCount(_primaryId) >= _primaryFramesAtRemoval + 4) {
            app->RequestExit();
            return;
        }

        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("runtime add/remove test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = surfaceId == _primaryId ? 0u : 2u;
        return packet;
    }

    void OnSurfaceAdded(Application* app, AppSurfaceId surfaceId, GpuSurface* surface) override {
        ColorClearCallbacksBase::OnSurfaceAdded(app, surfaceId, surface);
        if (surfaceId == _secondaryId) {
            _secondaryAdded = true;
        }
    }

    void OnSurfaceRemoved(Application* app, AppSurfaceId surfaceId, NativeWindow* window) override {
        ColorClearCallbacksBase::OnSurfaceRemoved(app, surfaceId, window);
        if (surfaceId == _secondaryId) {
            _secondaryRemoved = true;
            _primaryFramesAtRemoval = GetRenderCount(_primaryId);
        }
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }
    AppSurfaceId GetSecondaryId() const { return _secondaryId; }
    bool WasSecondaryAdded() const { return _secondaryAdded; }
    bool WasSecondaryRemoved() const { return _secondaryRemoved; }

private:
    NativeWindow* _secondaryWindow{nullptr};
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
    AppSurfaceId _secondaryId{AppSurfaceId::Invalid()};
    uint32_t _primaryFramesAtRemoval{0};
    bool _secondaryAdded{false};
    bool _removeRequested{false};
    bool _secondaryRemoved{false};
};

class RuntimeSlotReuseCallbacks : public ColorClearCallbacksBase {
public:
    explicit RuntimeSlotReuseCallbacks(NativeWindow* replacementWindow)
        : _replacementWindow(replacementWindow) {}

    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 2) {
            RecordFailure(fmt::format("expected 2 initial surfaces, got {}", ids.size()));
            return;
        }
        _primaryId = app->GetPrimarySurfaceId();
        _secondaryId = ids[0] == _primaryId ? ids[1] : ids[0];
    }

    void OnUpdate(Application* app, float dt) override {
        if (HasFailures()) {
            app->RequestExit();
            return;
        }

        if (!_swapRequested && GetRenderCount(_primaryId) >= 3 && GetRenderCount(_secondaryId) >= 3) {
            _swapRequested = true;
            app->RequestRemoveSurface(_secondaryId);

            AppSurfaceConfig config{};
            config.Window = _replacementWindow;
            config.SurfaceFormat = TextureFormat::BGRA8_UNORM;
            config.PresentMode = PresentMode::FIFO;
            config.BackBufferCount = 3;
            config.FlightFrameCount = 2;
            _replacementId = app->RequestAddSurface(config);
            if (_replacementId == AppSurfaceId::Invalid()) {
                RecordFailure("replacement add request returned an invalid surface id");
                app->RequestExit();
                return;
            }
            if (_replacementId != _secondaryId) {
                RecordFailure(fmt::format(
                    "expected replacement surface to reuse removed slot {}, got {}",
                    _secondaryId,
                    _replacementId));
                app->RequestExit();
                return;
            }
        }

        if (_replacementAdded) {
            auto ids = app->GetSurfaceIds();
            if (ids.size() != 2 ||
                std::find(ids.begin(), ids.end(), _primaryId) == ids.end() ||
                std::find(ids.begin(), ids.end(), _replacementId) == ids.end()) {
                RecordFailure("surface id set is inconsistent after slot reuse");
                app->RequestExit();
                return;
            }
        }

        if (_secondaryRemoved &&
            _replacementAdded &&
            GetRenderCount(_replacementId) >= _replacementRenderCountAtAdd + 2) {
            app->RequestExit();
            return;
        }

        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("runtime slot reuse test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = surfaceId == _primaryId ? 0u : 2u;
        return packet;
    }

    void OnSurfaceAdded(Application* app, AppSurfaceId surfaceId, GpuSurface* surface) override {
        ColorClearCallbacksBase::OnSurfaceAdded(app, surfaceId, surface);
        if (app->GetWindow(surfaceId) == _replacementWindow) {
            _replacementAdded = true;
            _replacementRenderCountAtAdd = GetRenderCount(surfaceId);
            if (surfaceId != _secondaryId) {
                RecordFailure(fmt::format(
                    "replacement surface expected id {}, got {}",
                    _secondaryId,
                    surfaceId));
            }
        }
    }

    void OnSurfaceRemoved(Application* app, AppSurfaceId surfaceId, NativeWindow* window) override {
        ColorClearCallbacksBase::OnSurfaceRemoved(app, surfaceId, window);
        if (surfaceId == _secondaryId) {
            _secondaryRemoved = true;
        }
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }
    AppSurfaceId GetSecondaryId() const { return _secondaryId; }
    AppSurfaceId GetReplacementId() const { return _replacementId; }
    bool ReplacementAdded() const { return _replacementAdded; }
    bool SecondaryRemoved() const { return _secondaryRemoved; }

private:
    NativeWindow* _replacementWindow{nullptr};
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
    AppSurfaceId _secondaryId{AppSurfaceId::Invalid()};
    AppSurfaceId _replacementId{AppSurfaceId::Invalid()};
    uint32_t _replacementRenderCountAtAdd{0};
    bool _swapRequested{false};
    bool _replacementAdded{false};
    bool _secondaryRemoved{false};
};

class RemovalSemanticsCallbacks : public ColorClearCallbacksBase {
public:
    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 2) {
            RecordFailure(fmt::format("expected 2 initial surfaces, got {}", ids.size()));
            return;
        }
        _primaryId = app->GetPrimarySurfaceId();
        _secondaryId = ids[0] == _primaryId ? ids[1] : ids[0];
    }

    void OnUpdate(Application* app, float dt) override {
        if (HasFailures()) {
            app->RequestExit();
            return;
        }

        if (!_secondaryRemoveRequested && GetRenderCount(_primaryId) >= 3 && GetRenderCount(_secondaryId) >= 3) {
            _secondaryRemoveRequested = true;
            app->RequestRemoveSurface(_secondaryId);
        }

        if (_secondaryRemoved && !_primaryRemoveRequested && GetRenderCount(_primaryId) >= _primaryFramesAfterSecondaryRemoval + 3) {
            _primaryRemoveRequested = true;
            app->RequestRemoveSurface(_primaryId);
            return;
        }

        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("surface removal semantics test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = surfaceId == _primaryId ? 0u : 1u;
        return packet;
    }

    void OnSurfaceRemoved(Application* app, AppSurfaceId surfaceId, NativeWindow* window) override {
        ColorClearCallbacksBase::OnSurfaceRemoved(app, surfaceId, window);
        if (surfaceId == _secondaryId) {
            _secondaryRemoved = true;
            _primaryFramesAfterSecondaryRemoval = GetRenderCount(_primaryId);
        }
        if (surfaceId == _primaryId) {
            _primaryRemovedCallbackSeen = true;
        }
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }
    AppSurfaceId GetSecondaryId() const { return _secondaryId; }
    bool SecondaryRemoved() const { return _secondaryRemoved; }
    bool PrimaryRemoveRequested() const { return _primaryRemoveRequested; }
    bool PrimaryRemovedCallbackSeen() const { return _primaryRemovedCallbackSeen; }

private:
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
    AppSurfaceId _secondaryId{AppSurfaceId::Invalid()};
    uint32_t _primaryFramesAfterSecondaryRemoval{0};
    bool _secondaryRemoveRequested{false};
    bool _secondaryRemoved{false};
    bool _primaryRemoveRequested{false};
    bool _primaryRemovedCallbackSeen{false};
};

class CloseSemanticsCallbacks : public ColorClearCallbacksBase {
public:
    CloseSemanticsCallbacks(NativeWindow* primaryWindow, NativeWindow* secondaryWindow)
        : _primaryWindow(primaryWindow),
          _secondaryWindow(secondaryWindow) {}

    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 2) {
            RecordFailure(fmt::format("expected 2 initial surfaces, got {}", ids.size()));
            return;
        }
        _primaryId = app->GetPrimarySurfaceId();
        _secondaryId = ids[0] == _primaryId ? ids[1] : ids[0];
    }

    void OnUpdate(Application* app, float dt) override {
        if (HasFailures()) {
            app->RequestExit();
            return;
        }

        if (!_secondaryClosePosted && GetRenderCount(_primaryId) >= 3 && GetRenderCount(_secondaryId) >= 3) {
            _secondaryClosePosted = true;
            if (!RequestNativeClose(_secondaryWindow)) {
                RecordFailure("failed to request secondary native window close");
                app->RequestExit();
                return;
            }
        }

        if (_secondaryRemoved &&
            !_primaryClosePosted &&
            GetRenderCount(_primaryId) >= _primaryFramesAfterSecondaryRemoval + 3) {
            _primaryClosePosted = true;
            if (!RequestNativeClose(_primaryWindow)) {
                RecordFailure("failed to request primary native window close");
                app->RequestExit();
                return;
            }
        }

        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("surface close semantics test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = surfaceId == _primaryId ? 0u : 1u;
        return packet;
    }

    void OnSurfaceRemoved(Application* app, AppSurfaceId surfaceId, NativeWindow* window) override {
        ColorClearCallbacksBase::OnSurfaceRemoved(app, surfaceId, window);
        if (surfaceId == _secondaryId) {
            _secondaryRemoved = true;
            _primaryFramesAfterSecondaryRemoval = GetRenderCount(_primaryId);
        }
        if (surfaceId == _primaryId) {
            _primaryRemovedCallbackSeen = true;
        }
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }
    AppSurfaceId GetSecondaryId() const { return _secondaryId; }
    bool SecondaryClosePosted() const { return _secondaryClosePosted; }
    bool SecondaryRemoved() const { return _secondaryRemoved; }
    bool PrimaryClosePosted() const { return _primaryClosePosted; }
    bool PrimaryRemovedCallbackSeen() const { return _primaryRemovedCallbackSeen; }

private:
    static bool RequestNativeClose(NativeWindow* window) {
        if (window == nullptr || !window->IsValid()) {
            return false;
        }

#if defined(_WIN32)
        auto handler = window->GetNativeHandler();
        if (handler.Type != WindowHandlerTag::HWND || handler.Handle == nullptr) {
            return false;
        }
        return ::PostMessageW(handler.Handle, kWin32WmClose, 0, 0) != 0;
#else
        (void)window;
        return false;
#endif
    }

    NativeWindow* _primaryWindow{nullptr};
    NativeWindow* _secondaryWindow{nullptr};
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
    AppSurfaceId _secondaryId{AppSurfaceId::Invalid()};
    uint32_t _primaryFramesAfterSecondaryRemoval{0};
    bool _secondaryClosePosted{false};
    bool _secondaryRemoved{false};
    bool _primaryClosePosted{false};
    bool _primaryRemovedCallbackSeen{false};
};

class RecreateCallbacks : public ColorClearCallbacksBase {
public:
    RecreateCallbacks(NativeWindow* secondaryWindow, int width, int height)
        : _secondaryWindow(secondaryWindow),
          _width(width),
          _height(height) {}

    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 2) {
            RecordFailure(fmt::format("expected 2 initial surfaces, got {}", ids.size()));
            return;
        }
        _primaryId = app->GetPrimarySurfaceId();
        _secondaryId = ids[0] == _primaryId ? ids[1] : ids[0];
    }

    void OnUpdate(Application* app, float dt) override {
        if (HasFailures()) {
            app->RequestExit();
            return;
        }

        if (!_resizeIssued && GetRenderCount(_primaryId) >= 3 && GetRenderCount(_secondaryId) >= 3) {
            _resizeIssued = true;
            _primaryFramesAtResize = GetRenderCount(_primaryId);
            _secondaryFramesAtResize = GetRenderCount(_secondaryId);
            _secondaryWindow->SetSize(_width, _height);
        }

        if (_resizeIssued &&
            GetRecreateCount(_secondaryId) >= 1 &&
            GetRenderCount(_primaryId) >= _primaryFramesAtResize + 3 &&
            GetRenderCount(_secondaryId) >= _secondaryFramesAtResize + 2) {
            app->RequestExit();
            return;
        }

        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("per-surface recreate test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = surfaceId == _primaryId ? 0u : 2u;
        return packet;
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }
    AppSurfaceId GetSecondaryId() const { return _secondaryId; }

private:
    NativeWindow* _secondaryWindow{nullptr};
    int _width{0};
    int _height{0};
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
    AppSurfaceId _secondaryId{AppSurfaceId::Invalid()};
    uint32_t _primaryFramesAtResize{0};
    uint32_t _secondaryFramesAtResize{0};
    bool _resizeIssued{false};
};

class QueuedAddCommandCoalescingCallbacks : public ColorClearCallbacksBase {
public:
    QueuedAddCommandCoalescingCallbacks(NativeWindow* cancelledWindow, NativeWindow* keptWindow)
        : _cancelledWindow(cancelledWindow),
          _keptWindow(keptWindow) {}

    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 1) {
            RecordFailure(fmt::format("expected 1 initial surface, got {}", ids.size()));
            return;
        }
        _primaryId = ids.front();
    }

    void OnUpdate(Application* app, float dt) override {
        if (HasFailures()) {
            app->RequestExit();
            return;
        }

        if (!_commandsQueued && app->GetFrameInfo().LogicFrameIndex >= 3) {
            AppSurfaceConfig cancelledConfig{};
            cancelledConfig.Window = _cancelledWindow;
            cancelledConfig.SurfaceFormat = TextureFormat::BGRA8_UNORM;
            cancelledConfig.PresentMode = PresentMode::FIFO;
            cancelledConfig.BackBufferCount = 3;
            cancelledConfig.FlightFrameCount = 2;
            _cancelledId = app->RequestAddSurface(cancelledConfig);
            if (_cancelledId == AppSurfaceId::Invalid()) {
                RecordFailure("cancelled add request returned an invalid surface id");
                app->RequestExit();
                return;
            }
            app->RequestRemoveSurface(_cancelledId);

            AppSurfaceConfig keptConfig{};
            keptConfig.Window = _keptWindow;
            keptConfig.SurfaceFormat = TextureFormat::BGRA8_UNORM;
            keptConfig.PresentMode = PresentMode::FIFO;
            keptConfig.BackBufferCount = 3;
            keptConfig.FlightFrameCount = 2;
            _keptId = app->RequestAddSurface(keptConfig);
            if (_keptId == AppSurfaceId::Invalid()) {
                RecordFailure("kept add request returned an invalid surface id");
                app->RequestExit();
                return;
            }
            app->RequestPresentModeChange(_keptId, PresentMode::Immediate);
            _commandsQueued = true;
        }

        if (_commandsQueued) {
            for (auto surfaceId : app->GetSurfaceIds()) {
                if (app->GetWindow(surfaceId) == _cancelledWindow) {
                    RecordFailure("cancelled queued add unexpectedly created a surface");
                    app->RequestExit();
                    return;
                }
            }
        }

        if (_keptAdded && GetRenderCount(_primaryId) >= 5 && GetRenderCount(_keptId) >= 3) {
            app->RequestExit();
            return;
        }

        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("queued add command coalescing test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = surfaceId == _primaryId ? 0u : 1u;
        return packet;
    }

    void OnSurfaceAdded(Application* app, AppSurfaceId surfaceId, GpuSurface* surface) override {
        ColorClearCallbacksBase::OnSurfaceAdded(app, surfaceId, surface);
        const auto* window = app->GetWindow(surfaceId);
        if (window == _cancelledWindow) {
            RecordFailure("cancelled queued add reached OnSurfaceAdded");
            return;
        }
        if (window == _keptWindow) {
            _keptAdded = true;
            if (surface == nullptr || surface->GetPresentMode() != PresentMode::Immediate) {
                RecordFailure("queued present-mode override was not applied to the added surface");
            }
        }
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }
    AppSurfaceId GetKeptId() const { return _keptId; }
    bool KeptSurfaceAdded() const { return _keptAdded; }

private:
    NativeWindow* _cancelledWindow{nullptr};
    NativeWindow* _keptWindow{nullptr};
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
    AppSurfaceId _cancelledId{AppSurfaceId::Invalid()};
    AppSurfaceId _keptId{AppSurfaceId::Invalid()};
    bool _commandsQueued{false};
    bool _keptAdded{false};
};

class ThreadModeCommandCoalescingCallbacks : public ColorClearCallbacksBase {
public:
    explicit ThreadModeCommandCoalescingCallbacks(bool initialMultiThreaded)
        : _initialMultiThreaded(initialMultiThreaded),
          _expectedMultiThreaded(!initialMultiThreaded) {}

    void OnStartup(Application* app) override {
        auto ids = app->GetSurfaceIds();
        if (ids.size() != 1) {
            RecordFailure(fmt::format("expected 1 initial surface, got {}", ids.size()));
            return;
        }
        _primaryId = ids.front();

        app->SetMultiThreadedRender(!_initialMultiThreaded);
        app->SetMultiThreadedRender(_initialMultiThreaded);
        app->SetMultiThreadedRender(_expectedMultiThreaded);
        _commandsQueued = true;
    }

    void OnUpdate(Application* app, float dt) override {
        if (HasFailures()) {
            app->RequestExit();
            return;
        }

        if (_commandsQueued && app->IsMultiThreadedRender() == _expectedMultiThreaded) {
            _observedExpectedMode = true;
        }

        if (_observedExpectedMode && GetRenderCount(_primaryId) >= 4) {
            app->RequestExit();
            return;
        }

        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            RecordFailure("thread mode command coalescing test timed out");
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        auto packet = make_unique<RGBPacket>();
        packet->ColorIndex = 2u;
        return packet;
    }

    AppSurfaceId GetPrimaryId() const { return _primaryId; }
    bool ObservedExpectedMode() const { return _observedExpectedMode; }
    bool ExpectedMultiThreaded() const { return _expectedMultiThreaded; }

private:
    bool _initialMultiThreaded{false};
    bool _expectedMultiThreaded{false};
    bool _commandsQueued{false};
    bool _observedExpectedMode{false};
    AppSurfaceId _primaryId{AppSurfaceId::Invalid()};
};

class UpdateExceptionCallbacks : public IAppCallbacks {
public:
    void OnUpdate(Application* app, float dt) override {
        (void)app;
        (void)dt;
        throw std::runtime_error("update exception");
    }
};

class RenderExceptionCallbacks : public IAppCallbacks {
public:
    void OnUpdate(Application* app, float dt) override {
        (void)dt;
        if (app->GetFrameInfo().LogicFrameIndex >= kMaxLogicFrames) {
            app->RequestExit();
        }
    }

    unique_ptr<FramePacket> OnExtractRenderData(
        Application* app,
        AppSurfaceId surfaceId,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo) override {
        (void)app;
        (void)surfaceId;
        (void)appInfo;
        (void)surfaceInfo;
        return make_unique<RGBPacket>();
    }

    void OnRender(
        Application* app,
        AppSurfaceId surfaceId,
        GpuFrameContext* frameCtx,
        const AppFrameInfo& appInfo,
        const AppSurfaceFrameInfo& surfaceInfo,
        FramePacket* packet) override {
        (void)app;
        (void)surfaceId;
        (void)frameCtx;
        (void)appInfo;
        (void)surfaceInfo;
        (void)packet;
        throw std::runtime_error("render exception");
    }
};

std::vector<RenderBackend> GetEnabledBackends() noexcept {
    std::vector<RenderBackend> backends{};
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.push_back(RenderBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    backends.push_back(RenderBackend::Vulkan);
#endif
    return backends;
}

const std::string* GetBackendSkipReason(RenderBackend backend) noexcept {
    struct BackendProbeState {
        std::once_flag Once{};
        bool Available{false};
        std::string Reason{};
    };

    static BackendProbeState d3d12State{};
    static BackendProbeState vulkanState{};

    auto probeD3D12 = []() {
        D3D12DeviceDescriptor desc{};
        desc.AdapterIndex = std::nullopt;
#ifdef RADRAY_IS_DEBUG
        desc.IsEnableDebugLayer = true;
        desc.IsEnableGpuBasedValid = true;
#endif
        auto runtimeOpt = GpuRuntime::Create(desc);
        return runtimeOpt.HasValue();
    };

    auto probeVulkan = []() {
        VulkanInstanceDescriptor instanceDesc{};
        instanceDesc.AppName = "ApplicationSmokeProbe";
        instanceDesc.AppVersion = 1;
        instanceDesc.EngineName = "RadRay";
        instanceDesc.EngineVersion = 1;
#ifdef RADRAY_IS_DEBUG
        instanceDesc.IsEnableDebugLayer = true;
#endif

        VulkanCommandQueueDescriptor queueDesc{};
        queueDesc.Type = QueueType::Direct;
        queueDesc.Count = 1;

        VulkanDeviceDescriptor deviceDesc{};
        deviceDesc.PhysicalDeviceIndex = std::nullopt;
        deviceDesc.Queues = std::span{&queueDesc, 1};

        auto runtimeOpt = GpuRuntime::Create(deviceDesc, instanceDesc);
        return runtimeOpt.HasValue();
    };

    switch (backend) {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
        case RenderBackend::D3D12:
            std::call_once(d3d12State.Once, [&]() {
                d3d12State.Available = probeD3D12();
                if (!d3d12State.Available) {
                    d3d12State.Reason = "GpuRuntime::Create(D3D12) failed";
                }
            });
            return d3d12State.Available ? nullptr : &d3d12State.Reason;
#endif
#if defined(RADRAY_ENABLE_VULKAN)
        case RenderBackend::Vulkan:
            std::call_once(vulkanState.Once, [&]() {
                vulkanState.Available = probeVulkan();
                if (!vulkanState.Available) {
                    vulkanState.Reason = "GpuRuntime::Create(Vulkan) failed";
                }
            });
            return vulkanState.Available ? nullptr : &vulkanState.Reason;
#endif
        default: {
            static const std::string unsupported = "Backend is not enabled for this build";
            return &unsupported;
        }
    }
}

std::string_view BackendName(RenderBackend backend) noexcept {
    switch (backend) {
        case RenderBackend::D3D12: return "D3D12";
        case RenderBackend::Vulkan: return "Vulkan";
        default: return "Unknown";
    }
}

std::vector<AppTestParam> GetApplicationParams() {
    std::vector<AppTestParam> params{};
    for (auto backend : GetEnabledBackends()) {
        params.push_back(AppTestParam{backend, false});
        params.push_back(AppTestParam{backend, true});
    }
    return params;
}

Nullable<unique_ptr<NativeWindow>> CreateTestWindow(std::string_view title, int x, int y, int width, int height, bool resizable) {
#if defined(_WIN32)
    Win32WindowCreateDescriptor windowDesc{};
    windowDesc.Title = title;
    windowDesc.Width = width;
    windowDesc.Height = height;
    windowDesc.X = x;
    windowDesc.Y = y;
    windowDesc.Resizable = resizable;
    NativeWindowCreateDescriptor nativeDesc = windowDesc;
    return CreateNativeWindow(nativeDesc);
#elif defined(__APPLE__)
    CocoaWindowCreateDescriptor windowDesc{};
    windowDesc.Title = title;
    windowDesc.Width = width;
    windowDesc.Height = height;
    windowDesc.X = x;
    windowDesc.Y = y;
    windowDesc.Resizable = resizable;
    NativeWindowCreateDescriptor nativeDesc = windowDesc;
    return CreateNativeWindow(nativeDesc);
#else
    (void)title;
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)resizable;
    return nullptr;
#endif
}

void DestroyWindowOwned(unique_ptr<NativeWindow>& window) {
    if (window != nullptr) {
        if (window->IsValid()) {
            window->Destroy();
        }
        window.reset();
    }
}

AppConfig MakeSingleSurfaceConfig(NativeWindow* window, const AppTestParam& param, uint32_t flightFrameCount = 2) {
    AppConfig config{};
    config.Backend = param.Backend;
    config.MultiThreadedRender = param.MultiThreaded;
    config.AllowFrameDrop = false;

    AppSurfaceConfig primary{};
    primary.Window = window;
    primary.SurfaceFormat = TextureFormat::BGRA8_UNORM;
    primary.PresentMode = PresentMode::FIFO;
    primary.BackBufferCount = 3;
    primary.FlightFrameCount = flightFrameCount;
    primary.IsPrimary = true;

    config.InitialSurfaces = {primary};
    return config;
}

AppConfig MakeTwoSurfaceConfig(NativeWindow* primaryWindow, NativeWindow* secondaryWindow, const AppTestParam& param) {
    AppConfig config{};
    config.Backend = param.Backend;
    config.MultiThreadedRender = param.MultiThreaded;
    config.AllowFrameDrop = false;

    AppSurfaceConfig primary{};
    primary.Window = primaryWindow;
    primary.SurfaceFormat = TextureFormat::BGRA8_UNORM;
    primary.PresentMode = PresentMode::FIFO;
    primary.BackBufferCount = 3;
    primary.FlightFrameCount = 2;
    primary.IsPrimary = true;

    AppSurfaceConfig secondary{};
    secondary.Window = secondaryWindow;
    secondary.SurfaceFormat = TextureFormat::BGRA8_UNORM;
    secondary.PresentMode = PresentMode::FIFO;
    secondary.BackBufferCount = 3;
    secondary.FlightFrameCount = 2;
    secondary.IsPrimary = false;

    config.InitialSurfaces = {primary, secondary};
    return config;
}

class ApplicationSmokeTest : public ::testing::TestWithParam<AppTestParam> {
protected:
    void SetUp() override {
        if (const auto* reason = GetBackendSkipReason(GetParam().Backend); reason != nullptr) {
            GTEST_SKIP() << *reason;
        }
    }
};

TEST_P(ApplicationSmokeTest, SingleSurfaceStillWorks) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto windowOpt = CreateTestWindow("ApplicationSmokeSingle", 120, 120, kInitialWidth, kInitialHeight, false);
    ASSERT_TRUE(windowOpt.HasValue()) << "Failed to create primary window";
    auto window = windowOpt.Release();

    SingleSurfaceCallbacks callbacks;
    Application app(MakeSingleSurfaceConfig(window.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(window);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_GT(callbacks.GetRenderCount(callbacks.GetPrimaryId()), 0u);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, SingleSurfaceRespectsFlightFrameCount) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    constexpr uint32_t kFlightFrameCount = 3;

    auto windowOpt = CreateTestWindow("ApplicationMultiInFlight", 130, 130, kInitialWidth, kInitialHeight, false);
    ASSERT_TRUE(windowOpt.HasValue()) << "Failed to create primary window";
    auto window = windowOpt.Release();

    MultiInFlightCallbacks callbacks(kFlightFrameCount, param.MultiThreaded);
    Application app(MakeSingleSurfaceConfig(window.get(), param, kFlightFrameCount), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(window);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetPrimaryId()), kFlightFrameCount);
    if (param.MultiThreaded) {
        EXPECT_GE(callbacks.GetMaxOutstanding(), kFlightFrameCount);
    } else {
        EXPECT_LE(callbacks.GetMaxOutstanding(), 1u);
    }
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, MultiSurfaceRoundRobin) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto primaryOpt = CreateTestWindow("ApplicationSmokePrimary", 120, 120, kInitialWidth, kInitialHeight, false);
    auto secondaryOpt = CreateTestWindow("ApplicationSmokeSecondary", 820, 120, kSecondaryWidth, kSecondaryHeight, false);
    ASSERT_TRUE(primaryOpt.HasValue()) << "Failed to create primary window";
    ASSERT_TRUE(secondaryOpt.HasValue()) << "Failed to create secondary window";
    auto primary = primaryOpt.Release();
    auto secondary = secondaryOpt.Release();

    MultiSurfaceCallbacks callbacks;
    Application app(MakeTwoSurfaceConfig(primary.get(), secondary.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(secondary);
    DestroyWindowOwned(primary);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetPrimaryId()), 6u);
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetSecondaryId()), 6u);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, RuntimeAddRemoveSurface) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto primaryOpt = CreateTestWindow("ApplicationAddRemovePrimary", 140, 140, kInitialWidth, kInitialHeight, false);
    auto secondaryOpt = CreateTestWindow("ApplicationAddRemoveSecondary", 860, 140, kSecondaryWidth, kSecondaryHeight, false);
    ASSERT_TRUE(primaryOpt.HasValue()) << "Failed to create primary window";
    ASSERT_TRUE(secondaryOpt.HasValue()) << "Failed to create secondary window";
    auto primary = primaryOpt.Release();
    auto secondary = secondaryOpt.Release();

    RuntimeAddRemoveCallbacks callbacks(secondary.get());
    Application app(MakeSingleSurfaceConfig(primary.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(secondary);
    DestroyWindowOwned(primary);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_TRUE(callbacks.WasSecondaryAdded());
    EXPECT_TRUE(callbacks.WasSecondaryRemoved());
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetPrimaryId()), 6u);
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetSecondaryId()), 4u);
    EXPECT_EQ(callbacks.GetAddedCount(callbacks.GetSecondaryId()), 1u);
    EXPECT_EQ(callbacks.GetRemovedCount(callbacks.GetSecondaryId()), 1u);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, RuntimeAddRemoveReusesFreedSlot) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto primaryOpt = CreateTestWindow("ApplicationSlotReusePrimary", 150, 150, kInitialWidth, kInitialHeight, false);
    auto secondaryOpt = CreateTestWindow("ApplicationSlotReuseSecondary", 870, 150, kSecondaryWidth, kSecondaryHeight, false);
    auto replacementOpt = CreateTestWindow("ApplicationSlotReuseReplacement", 1370, 150, kSecondaryWidth, kSecondaryHeight, false);
    ASSERT_TRUE(primaryOpt.HasValue()) << "Failed to create primary window";
    ASSERT_TRUE(secondaryOpt.HasValue()) << "Failed to create secondary window";
    ASSERT_TRUE(replacementOpt.HasValue()) << "Failed to create replacement window";
    auto primary = primaryOpt.Release();
    auto secondary = secondaryOpt.Release();
    auto replacement = replacementOpt.Release();

    RuntimeSlotReuseCallbacks callbacks(replacement.get());
    Application app(MakeTwoSurfaceConfig(primary.get(), secondary.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(replacement);
    DestroyWindowOwned(secondary);
    DestroyWindowOwned(primary);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_TRUE(callbacks.SecondaryRemoved());
    EXPECT_TRUE(callbacks.ReplacementAdded());
    EXPECT_EQ(callbacks.GetReplacementId(), callbacks.GetSecondaryId());
    EXPECT_EQ(callbacks.GetAddedCount(callbacks.GetReplacementId()), 1u);
    EXPECT_EQ(callbacks.GetRemovedCount(callbacks.GetReplacementId()), 1u);
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetPrimaryId()), 5u);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, SurfaceRemovalSemantics) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto primaryOpt = CreateTestWindow("ApplicationRemovalPrimary", 160, 160, kInitialWidth, kInitialHeight, false);
    auto secondaryOpt = CreateTestWindow("ApplicationRemovalSecondary", 900, 160, kSecondaryWidth, kSecondaryHeight, false);
    ASSERT_TRUE(primaryOpt.HasValue()) << "Failed to create primary window";
    ASSERT_TRUE(secondaryOpt.HasValue()) << "Failed to create secondary window";
    auto primary = primaryOpt.Release();
    auto secondary = secondaryOpt.Release();

    RemovalSemanticsCallbacks callbacks;
    Application app(MakeTwoSurfaceConfig(primary.get(), secondary.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(secondary);
    DestroyWindowOwned(primary);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_TRUE(callbacks.SecondaryRemoved());
    EXPECT_TRUE(callbacks.PrimaryRemoveRequested());
    EXPECT_EQ(callbacks.GetRemovedCount(callbacks.GetSecondaryId()), 1u);
    EXPECT_EQ(callbacks.GetRemovedCount(callbacks.GetPrimaryId()), 0u);
    EXPECT_FALSE(callbacks.PrimaryRemovedCallbackSeen());
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, SurfaceCloseSemantics) {
#if !defined(_WIN32)
    GTEST_SKIP() << "Native close signaling is only wired for Win32 in this smoke test.";
#endif

    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto primaryOpt = CreateTestWindow("ApplicationClosePrimary", 170, 170, kInitialWidth, kInitialHeight, false);
    auto secondaryOpt = CreateTestWindow("ApplicationCloseSecondary", 920, 170, kSecondaryWidth, kSecondaryHeight, false);
    ASSERT_TRUE(primaryOpt.HasValue()) << "Failed to create primary window";
    ASSERT_TRUE(secondaryOpt.HasValue()) << "Failed to create secondary window";
    auto primary = primaryOpt.Release();
    auto secondary = secondaryOpt.Release();

    CloseSemanticsCallbacks callbacks(primary.get(), secondary.get());
    Application app(MakeTwoSurfaceConfig(primary.get(), secondary.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(secondary);
    DestroyWindowOwned(primary);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_TRUE(callbacks.SecondaryClosePosted());
    EXPECT_TRUE(callbacks.SecondaryRemoved());
    EXPECT_TRUE(callbacks.PrimaryClosePosted());
    EXPECT_EQ(callbacks.GetRemovedCount(callbacks.GetSecondaryId()), 1u);
    EXPECT_EQ(callbacks.GetRemovedCount(callbacks.GetPrimaryId()), 0u);
    EXPECT_FALSE(callbacks.PrimaryRemovedCallbackSeen());
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, PerSurfaceRecreate) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto primaryOpt = CreateTestWindow("ApplicationRecreatePrimary", 180, 180, kInitialWidth, kInitialHeight, false);
    auto secondaryOpt = CreateTestWindow("ApplicationRecreateSecondary", 940, 180, kSecondaryWidth, kSecondaryHeight, true);
    ASSERT_TRUE(primaryOpt.HasValue()) << "Failed to create primary window";
    ASSERT_TRUE(secondaryOpt.HasValue()) << "Failed to create secondary window";
    auto primary = primaryOpt.Release();
    auto secondary = secondaryOpt.Release();

    RecreateCallbacks callbacks(secondary.get(), kSecondaryWidth + 96, kSecondaryHeight + 64);
    Application app(MakeTwoSurfaceConfig(primary.get(), secondary.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(secondary);
    DestroyWindowOwned(primary);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_EQ(callbacks.GetRecreateCount(callbacks.GetPrimaryId()), 0u);
    EXPECT_GE(callbacks.GetRecreateCount(callbacks.GetSecondaryId()), 1u);
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetPrimaryId()), 6u);
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetSecondaryId()), 5u);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, QueuedAddCommandCoalescing) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto primaryOpt = CreateTestWindow("ApplicationQueuedAddPrimary", 200, 200, kInitialWidth, kInitialHeight, false);
    auto cancelledOpt = CreateTestWindow("ApplicationQueuedAddCancelled", 960, 200, kSecondaryWidth, kSecondaryHeight, false);
    auto keptOpt = CreateTestWindow("ApplicationQueuedAddKept", 1460, 200, kSecondaryWidth, kSecondaryHeight, false);
    ASSERT_TRUE(primaryOpt.HasValue()) << "Failed to create primary window";
    ASSERT_TRUE(cancelledOpt.HasValue()) << "Failed to create cancelled secondary window";
    ASSERT_TRUE(keptOpt.HasValue()) << "Failed to create kept secondary window";
    auto primary = primaryOpt.Release();
    auto cancelled = cancelledOpt.Release();
    auto kept = keptOpt.Release();

    QueuedAddCommandCoalescingCallbacks callbacks(cancelled.get(), kept.get());
    Application app(MakeSingleSurfaceConfig(primary.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(kept);
    DestroyWindowOwned(cancelled);
    DestroyWindowOwned(primary);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_TRUE(callbacks.KeptSurfaceAdded());
    EXPECT_EQ(callbacks.GetAddedCount(callbacks.GetKeptId()), 1u);
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetPrimaryId()), 5u);
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetKeptId()), 3u);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, ThreadModeCommandCoalescing) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto windowOpt = CreateTestWindow("ApplicationThreadModeCoalescing", 220, 220, kInitialWidth, kInitialHeight, false);
    ASSERT_TRUE(windowOpt.HasValue()) << "Failed to create primary window";
    auto window = windowOpt.Release();

    ThreadModeCommandCoalescingCallbacks callbacks(param.MultiThreaded);
    Application app(MakeSingleSurfaceConfig(window.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto failures = callbacks.GetFailures();
    auto errors = logs.GetErrors();

    DestroyWindowOwned(window);

    EXPECT_EQ(exitCode, 0);
    EXPECT_TRUE(failures.empty()) << (failures.empty() ? "" : failures.front());
    EXPECT_TRUE(callbacks.ObservedExpectedMode());
    EXPECT_GE(callbacks.GetRenderCount(callbacks.GetPrimaryId()), 4u);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
}

TEST_P(ApplicationSmokeTest, UpdateCallbackExceptionIsCaught) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto windowOpt = CreateTestWindow("ApplicationUpdateException", 240, 240, kInitialWidth, kInitialHeight, false);
    ASSERT_TRUE(windowOpt.HasValue()) << "Failed to create primary window";
    auto window = windowOpt.Release();

    UpdateExceptionCallbacks callbacks;
    Application app(MakeSingleSurfaceConfig(window.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto errors = logs.GetErrors();

    DestroyWindowOwned(window);

    EXPECT_EQ(exitCode, 1);
    EXPECT_TRUE(ContainsErrorMessage(errors, "IAppCallbacks::OnUpdate threw"));
}

TEST_P(ApplicationSmokeTest, RenderCallbackExceptionIsCaught) {
    const auto param = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback logScope(&logs);

    auto windowOpt = CreateTestWindow("ApplicationRenderException", 260, 260, kInitialWidth, kInitialHeight, false);
    ASSERT_TRUE(windowOpt.HasValue()) << "Failed to create primary window";
    auto window = windowOpt.Release();

    RenderExceptionCallbacks callbacks;
    Application app(MakeSingleSurfaceConfig(window.get(), param), &callbacks);
    const int exitCode = app.Run();

    auto errors = logs.GetErrors();

    DestroyWindowOwned(window);

    EXPECT_EQ(exitCode, 1);
    EXPECT_TRUE(ContainsErrorMessage(errors, "IAppCallbacks::OnRender threw"));
}

INSTANTIATE_TEST_SUITE_P(
    ApplicationSmoke,
    ApplicationSmokeTest,
    ::testing::ValuesIn(GetApplicationParams()),
    [](const ::testing::TestParamInfo<AppTestParam>& info) {
        return fmt::format("{}_{}", BackendName(info.param.Backend), info.param.MultiThreaded ? "MultiThreaded" : "SingleThreaded");
    });

}  // namespace
