#include <radray/runtime/render_system.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <radray/logger.h>

namespace radray {

namespace detail {

enum class RenderFrameLifecycle {
    Recording,
    Submitted,
    Cancelled,
    Completed,
};

struct RenderFrameState {
    std::mutex Mutex{};
    std::condition_variable CompletionCv{};
    weak_ptr<RenderServiceState> Service{};
    FrameId Id{};
    RenderFrameLifecycle Lifecycle{RenderFrameLifecycle::Recording};
    unique_ptr<render::CommandBuffer> CommandBuffer{};
    std::optional<render::CBufferArena> CBufferArena{};
    RenderFrameTransientAllocator TransientAllocator{};
    RenderFrameStats Stats{};
    vector<std::function<void()>> DeferredRelease{};
    RenderFrameCompletionHooks CompletionHooks{};
    std::exception_ptr Error{};
    uint64_t NextCallbackId{1};
    vector<std::pair<uint64_t, std::function<void()>>> CompletionCallbacks{};
};

struct RenderServiceState {
    std::mutex Mutex{};
    RenderServiceDescriptor Desc{};
    uint64_t NextFrameId{1};
    deque<shared_ptr<RenderFrameState>> SubmittedFrames{};
};

struct CleanupPayload {
    unique_ptr<render::CommandBuffer> CommandBuffer{};
    std::optional<render::CBufferArena> CBufferArena{};
    RenderFrameTransientAllocator TransientAllocator{};
    vector<std::function<void()>> DeferredRelease{};
};

void CaptureFirstError(std::exception_ptr& dst, std::exception_ptr src) noexcept {
    if (!dst && src) {
        dst = std::move(src);
    }
}

void CaptureFrameError(const shared_ptr<RenderFrameState>& frame, std::exception_ptr error) noexcept {
    if (!frame || !error) {
        return;
    }

    std::lock_guard lock(frame->Mutex);
    CaptureFirstError(frame->Error, std::move(error));
}

bool IsCompletedUnlocked(const RenderFrameState& frame) noexcept {
    return frame.Lifecycle == RenderFrameLifecycle::Completed;
}

bool IsRecordingUnlocked(const RenderFrameState& frame) noexcept {
    return frame.Lifecycle == RenderFrameLifecycle::Recording;
}

bool IsSubmittedUnlocked(const RenderFrameState& frame) noexcept {
    return frame.Lifecycle == RenderFrameLifecycle::Submitted;
}

RenderFrameCompletionHooks CopyCompletionHooks(const shared_ptr<RenderFrameState>& frame) {
    std::lock_guard lock(frame->Mutex);
    return frame->CompletionHooks;
}

bool PollFrameReady(const shared_ptr<RenderFrameState>& frame) noexcept {
    RenderFrameCompletionHooks hooks = CopyCompletionHooks(frame);
    if (!hooks.IsReady) {
        return true;
    }

    try {
        return hooks.IsReady();
    } catch (...) {
        CaptureFrameError(frame, std::current_exception());
        return true;
    }
}

void WaitUntilFrameReady(const shared_ptr<RenderFrameState>& frame) noexcept {
    for (;;) {
        if (PollFrameReady(frame)) {
            return;
        }

        RenderFrameCompletionHooks hooks = CopyCompletionHooks(frame);
        if (hooks.Wait) {
            try {
                hooks.Wait();
            } catch (...) {
                CaptureFrameError(frame, std::current_exception());
                return;
            }
        } else {
            std::this_thread::yield();
        }
    }
}

std::exception_ptr QueryFrameError(const shared_ptr<RenderFrameState>& frame) noexcept {
    RenderFrameCompletionHooks hooks = CopyCompletionHooks(frame);
    if (!hooks.GetError) {
        return nullptr;
    }

    try {
        return hooks.GetError();
    } catch (...) {
        return std::current_exception();
    }
}

CleanupPayload ExtractCleanupPayload(
    const shared_ptr<RenderFrameState>& frame,
    bool resetStats) noexcept {
    std::lock_guard lock(frame->Mutex);

    CleanupPayload payload{};
    payload.CommandBuffer = std::move(frame->CommandBuffer);
    if (frame->CBufferArena) {
        payload.CBufferArena.emplace(std::move(*frame->CBufferArena));
        frame->CBufferArena.reset();
    }
    payload.TransientAllocator = std::move(frame->TransientAllocator);
    frame->TransientAllocator = RenderFrameTransientAllocator{};
    payload.DeferredRelease = std::move(frame->DeferredRelease);
    frame->CompletionHooks = {};
    if (resetStats) {
        frame->Stats = {};
    }
    return payload;
}

std::exception_ptr RunCleanup(CleanupPayload payload) noexcept {
    std::exception_ptr error{};

    if (payload.CBufferArena) {
        try {
            payload.CBufferArena->Clear();
        } catch (...) {
            CaptureFirstError(error, std::current_exception());
        }
    }

    try {
        payload.TransientAllocator.Clear();
    } catch (...) {
        CaptureFirstError(error, std::current_exception());
    }

    for (auto& callback : payload.DeferredRelease) {
        if (!callback) {
            continue;
        }
        try {
            callback();
        } catch (...) {
            CaptureFirstError(error, std::current_exception());
        }
    }

    payload.CommandBuffer.reset();
    return error;
}

void CompleteFrame(
    const shared_ptr<RenderFrameState>& frame,
    std::exception_ptr error) noexcept {
    vector<std::function<void()>> callbacks{};
    {
        std::lock_guard lock(frame->Mutex);
        CaptureFirstError(frame->Error, std::move(error));
        frame->Lifecycle = RenderFrameLifecycle::Completed;
        callbacks.reserve(frame->CompletionCallbacks.size());
        for (auto& entry : frame->CompletionCallbacks) {
            callbacks.push_back(std::move(entry.second));
        }
        frame->CompletionCallbacks.clear();
    }

    frame->CompletionCv.notify_all();
    for (auto& callback : callbacks) {
        if (!callback) {
            continue;
        }
        callback();
    }
}

void CancelFrame(const shared_ptr<RenderFrameState>& frame) noexcept {
    if (!frame) {
        return;
    }

    {
        std::lock_guard lock(frame->Mutex);
        if (!IsRecordingUnlocked(*frame)) {
            return;
        }
        frame->Lifecycle = RenderFrameLifecycle::Cancelled;
    }

    auto payload = ExtractCleanupPayload(frame, true);
    auto error = RunCleanup(std::move(payload));
    if (error) {
        CaptureFrameError(frame, error);
    }
}

shared_ptr<RenderFrameState> PeekFrontFrame(const shared_ptr<RenderServiceState>& service) noexcept {
    std::lock_guard lock(service->Mutex);
    if (service->SubmittedFrames.empty()) {
        return nullptr;
    }
    return service->SubmittedFrames.front();
}

bool PopFrontFrameIfMatches(
    const shared_ptr<RenderServiceState>& service,
    const shared_ptr<RenderFrameState>& frame) noexcept {
    std::lock_guard lock(service->Mutex);
    if (service->SubmittedFrames.empty()) {
        return false;
    }
    if (service->SubmittedFrames.front().get() != frame.get()) {
        return false;
    }
    service->SubmittedFrames.pop_front();
    return true;
}

bool TryRetireFrontFrame(
    const shared_ptr<RenderServiceState>& service,
    bool waitForReady) noexcept {
    auto frame = PeekFrontFrame(service);
    if (!frame) {
        return false;
    }

    if (waitForReady) {
        WaitUntilFrameReady(frame);
    } else if (!PollFrameReady(frame)) {
        return false;
    }

    auto hookError = QueryFrameError(frame);
    if (!PopFrontFrameIfMatches(service, frame)) {
        return true;
    }

    auto payload = ExtractCleanupPayload(frame, true);
    auto cleanupError = RunCleanup(std::move(payload));
    CaptureFirstError(hookError, std::move(cleanupError));
    CompleteFrame(frame, std::move(hookError));
    return true;
}

void CollectRetiredFramesImpl(const shared_ptr<RenderServiceState>& service) noexcept {
    if (!service) {
        return;
    }

    while (TryRetireFrontFrame(service, false)) {
    }
}

void WaitForAllFramesImpl(const shared_ptr<RenderServiceState>& service) noexcept {
    if (!service) {
        return;
    }

    while (PeekFrontFrame(service)) {
        TryRetireFrontFrame(service, true);
    }
}

void WaitForFrameImpl(
    const shared_ptr<RenderServiceState>& service,
    const shared_ptr<RenderFrameState>& target) noexcept {
    if (!service || !target) {
        return;
    }

    for (;;) {
        {
            std::lock_guard lock(target->Mutex);
            if (IsCompletedUnlocked(*target)) {
                return;
            }
        }

        auto front = PeekFrontFrame(service);
        if (!front) {
            break;
        }

        TryRetireFrontFrame(service, true);
    }
}

void EnsureServiceState(const shared_ptr<RenderServiceState>& state) {
    if (!state) {
        throw std::logic_error("RenderService is not initialized");
    }
}

shared_ptr<RenderFrameState> RequireRecordingFrameState(const shared_ptr<RenderFrameState>& state) {
    if (!state) {
        throw std::logic_error("RenderFrame is invalid");
    }

    std::lock_guard lock(state->Mutex);
    if (!IsRecordingUnlocked(*state)) {
        throw std::logic_error("RenderFrame is not in recording state");
    }
    return state;
}

CompletionCallbackHandle RegisterFrameCompletionCallback(
    const shared_ptr<RenderFrameState>& frame,
    std::function<void()> callback) noexcept {
    if (!frame || !callback) {
        return {};
    }

    CompletionCallbackHandle handle{};
    {
        std::lock_guard lock(frame->Mutex);
        if (IsCompletedUnlocked(*frame)) {
            return {};
        }
        handle.Id = frame->NextCallbackId++;
        frame->CompletionCallbacks.emplace_back(handle.Id, std::move(callback));
    }
    return handle;
}

void UnregisterFrameCompletionCallback(
    const weak_ptr<RenderFrameState>& frame,
    CompletionCallbackHandle handle) noexcept {
    if (!handle.IsValid()) {
        return;
    }

    auto sharedFrame = frame.lock();
    if (!sharedFrame) {
        return;
    }

    std::lock_guard lock(sharedFrame->Mutex);
    if (IsCompletedUnlocked(*sharedFrame)) {
        return;
    }

    auto it = std::remove_if(
        sharedFrame->CompletionCallbacks.begin(),
        sharedFrame->CompletionCallbacks.end(),
        [handle](const auto& entry) { return entry.first == handle.Id; });
    sharedFrame->CompletionCallbacks.erase(it, sharedFrame->CompletionCallbacks.end());
}

void ObserveFrameCompletion(const shared_ptr<RenderFrameState>& frame) noexcept {
    if (!frame) {
        return;
    }

    {
        std::lock_guard lock(frame->Mutex);
        if (IsCompletedUnlocked(*frame)) {
            return;
        }
    }

    auto service = frame->Service.lock();
    if (service) {
        CollectRetiredFramesImpl(service);
    }
}

void WaitForFrameCompletion(const shared_ptr<RenderFrameState>& frame) noexcept {
    if (!frame) {
        return;
    }

    ObserveFrameCompletion(frame);

    {
        std::lock_guard lock(frame->Mutex);
        if (IsCompletedUnlocked(*frame)) {
            return;
        }
    }

    auto service = frame->Service.lock();
    if (service) {
        WaitForFrameImpl(service, frame);
    }

    std::unique_lock lock(frame->Mutex);
    frame->CompletionCv.wait(lock, [&]() noexcept {
        return IsCompletedUnlocked(*frame);
    });
}

bool IsFrameComplete(const shared_ptr<RenderFrameState>& frame) noexcept {
    if (!frame) {
        return true;
    }

    std::lock_guard lock(frame->Mutex);
    return IsCompletedUnlocked(*frame);
}

std::exception_ptr GetFrameError(const shared_ptr<RenderFrameState>& frame) noexcept {
    if (!frame) {
        return nullptr;
    }

    std::lock_guard lock(frame->Mutex);
    if (!IsCompletedUnlocked(*frame)) {
        return nullptr;
    }
    return frame->Error;
}

}  // namespace detail

RenderFrameTransientAllocator::RenderFrameTransientAllocator(const Descriptor& desc) noexcept
    : _desc(desc) {}

void* RenderFrameTransientAllocator::Allocate(size_t size, size_t alignment) {
    if (size == 0) {
        return nullptr;
    }

    if (alignment == 0) {
        alignment = 1;
    }

    size_t alignedOffset = _used;
    const size_t remainder = alignedOffset % alignment;
    if (remainder != 0) {
        alignedOffset += alignment - remainder;
    }

    const size_t requiredSize = alignedOffset + size;
    EnsureCapacity(requiredSize);

    void* ptr = _storage.data() + alignedOffset;
    _used = requiredSize;
    return ptr;
}

void RenderFrameTransientAllocator::Reset() noexcept {
    _used = 0;
}

void RenderFrameTransientAllocator::Clear() noexcept {
    _storage.clear();
    _used = 0;
}

void RenderFrameTransientAllocator::EnsureCapacity(size_t requiredSize) {
    if (requiredSize <= _storage.size()) {
        return;
    }

    size_t newCapacity = _storage.size();
    if (newCapacity == 0) {
        newCapacity = std::max<size_t>(_desc.InitialCapacity, requiredSize);
    }
    while (newCapacity < requiredSize) {
        newCapacity = std::max(requiredSize, newCapacity * 2);
    }
    _storage.resize(newCapacity);
}

RenderService::RenderService()
    : RenderService(RenderServiceDescriptor{}) {}

RenderService::RenderService(const RenderServiceDescriptor& desc)
    : _state(make_shared<detail::RenderServiceState>()) {
    _state->Desc = desc;
}

RenderService::RenderService(shared_ptr<detail::RenderServiceState> state) noexcept
    : _state(std::move(state)) {}

RenderService::RenderService(RenderService&& other) noexcept
    : _state(std::move(other._state)) {}

RenderService& RenderService::operator=(RenderService&& other) noexcept {
    if (this != &other) {
        Shutdown();
        _state = std::move(other._state);
    }
    return *this;
}

RenderService::~RenderService() noexcept {
    Shutdown();
}

RenderFrame RenderService::BeginFrame() {
    detail::EnsureServiceState(_state);
    CollectRetiredFrames();

    auto frameState = make_shared<detail::RenderFrameState>();
    frameState->Service = _state;
    frameState->TransientAllocator = RenderFrameTransientAllocator{
        _state->Desc.TransientAllocator};
    {
        std::lock_guard lock(_state->Mutex);
        frameState->Id = FrameId{_state->NextFrameId++};
    }
    if (_state->Desc.Device) {
        frameState->CBufferArena.emplace(
            _state->Desc.Device.get(),
            _state->Desc.CBufferArena);
    }
    return RenderFrame{std::move(frameState)};
}

void RenderService::CollectRetiredFrames() noexcept {
    detail::CollectRetiredFramesImpl(_state);
}

void RenderService::WaitIdle() noexcept {
    detail::WaitForAllFramesImpl(_state);
}

void RenderService::Shutdown() noexcept {
    if (!_state) {
        return;
    }

    WaitIdle();
    _state.reset();
}

Nullable<render::Device*> RenderService::GetDevice() const noexcept {
    if (!_state || !_state->Desc.Device) {
        return nullptr;
    }
    return _state->Desc.Device.get();
}

RenderFrame::RenderFrame(shared_ptr<detail::RenderFrameState> state) noexcept
    : _state(std::move(state)) {}

RenderFrame::RenderFrame(RenderFrame&& other) noexcept
    : _state(std::move(other._state)) {}

RenderFrame& RenderFrame::operator=(RenderFrame&& other) noexcept {
    if (this != &other) {
        Cancel();
        _state = std::move(other._state);
    }
    return *this;
}

RenderFrame::~RenderFrame() noexcept {
    Cancel();
}

bool RenderFrame::IsValid() const noexcept {
    if (!_state) {
        return false;
    }

    std::lock_guard lock(_state->Mutex);
    return detail::IsRecordingUnlocked(*_state);
}

FrameId RenderFrame::GetFrameId() const noexcept {
    if (!_state) {
        return {};
    }
    return _state->Id;
}

Nullable<render::CommandBuffer*> RenderFrame::GetCommandBuffer() const noexcept {
    if (!_state) {
        return nullptr;
    }
    return _state->CommandBuffer.get();
}

void RenderFrame::SetCommandBuffer(
    Nullable<unique_ptr<render::CommandBuffer>> commandBuffer) noexcept {
    auto state = detail::RequireRecordingFrameState(_state);
    state->CommandBuffer = commandBuffer.Release();
}

Nullable<const render::CBufferArena*> RenderFrame::GetCBufferArena() const noexcept {
    if (!_state || !_state->CBufferArena) {
        return nullptr;
    }
    return &*_state->CBufferArena;
}

render::CBufferArena::Allocation RenderFrame::AllocateConstantBuffer(uint64_t size) noexcept {
    auto state = detail::RequireRecordingFrameState(_state);
    if (!state->CBufferArena) {
        return render::CBufferArena::Allocation::Invalid();
    }

    auto allocation = state->CBufferArena->Allocate(size);
    if (allocation.Target != nullptr) {
        state->Stats.ConstantBytesAllocated += allocation.Size;
    }
    return allocation;
}

void* RenderFrame::AllocateTransient(size_t size, size_t alignment) {
    auto state = detail::RequireRecordingFrameState(_state);
    void* ptr = state->TransientAllocator.Allocate(size, alignment);
    if (ptr != nullptr) {
        state->Stats.TransientBytesAllocated += size;
    }
    return ptr;
}

const RenderFrameTransientAllocator& RenderFrame::GetTransientAllocator() const noexcept {
    RADRAY_ASSERT(_state != nullptr);
    return _state->TransientAllocator;
}

RenderFrameStats& RenderFrame::GetStats() noexcept {
    RADRAY_ASSERT(_state != nullptr);
    return _state->Stats;
}

const RenderFrameStats& RenderFrame::GetStats() const noexcept {
    RADRAY_ASSERT(_state != nullptr);
    return _state->Stats;
}

void RenderFrame::SetCompletionHooks(RenderFrameCompletionHooks hooks) {
    auto state = detail::RequireRecordingFrameState(_state);
    state->CompletionHooks = std::move(hooks);
}

void RenderFrame::DeferRelease(std::function<void()> callback) {
    auto state = detail::RequireRecordingFrameState(_state);
    state->DeferredRelease.push_back(std::move(callback));
    state->Stats.DeferredReleaseCount = static_cast<uint64_t>(state->DeferredRelease.size());
}

void RenderFrame::Cancel() noexcept {
    if (!_state) {
        return;
    }

    auto state = std::exchange(_state, nullptr);
    detail::CancelFrame(std::move(state));
}

RenderFrameTicket RenderFrame::Submit() & {
    return SubmitImpl();
}

RenderFrameTicket RenderFrame::Submit() && {
    return SubmitImpl();
}

RenderFrameTicket RenderFrame::SubmitImpl() {
    auto state = detail::RequireRecordingFrameState(_state);
    auto service = state->Service.lock();
    detail::EnsureServiceState(service);

    {
        std::lock_guard lock(state->Mutex);
        state->Lifecycle = detail::RenderFrameLifecycle::Submitted;
    }
    {
        std::lock_guard lock(service->Mutex);
        service->SubmittedFrames.push_back(state);
    }

    _state.reset();
    return RenderFrameTicket{state};
}

FrameId RenderFrameTicket::GetFrameId() const noexcept {
    if (!_state) {
        return {};
    }
    return _state->Id;
}

bool RenderFrameTicket::IsComplete() const noexcept {
    if (!_state) {
        return true;
    }

    detail::ObserveFrameCompletion(_state);
    return detail::IsFrameComplete(_state);
}

bool RenderFrameTicket::HasError() const noexcept {
    if (!_state) {
        return false;
    }

    detail::ObserveFrameCompletion(_state);
    return detail::GetFrameError(_state) != nullptr;
}

std::exception_ptr RenderFrameTicket::GetError() const noexcept {
    if (!_state) {
        return nullptr;
    }

    detail::ObserveFrameCompletion(_state);
    return detail::GetFrameError(_state);
}

void RenderFrameTicket::Wait() const {
    if (!_state) {
        return;
    }

    detail::WaitForFrameCompletion(_state);
    if (auto error = detail::GetFrameError(_state)) {
        std::rethrow_exception(error);
    }
}

}  // namespace radray
