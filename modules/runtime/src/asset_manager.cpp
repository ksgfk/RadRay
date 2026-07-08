#include <radray/runtime/asset_manager.h>

#include <exception>

#include <radray/logger.h>
#include <radray/render/common.h>

namespace radray {

namespace {

class ImmediateRenderResourceRecycler final : public IRenderResourceRecycler {
public:
    void RecycleRenderResource(unique_ptr<render::RenderBase> obj) noexcept override {
        obj.reset();
    }
};

}  // namespace

class AssetWaitAwaitable {
public:
    AssetWaitAwaitable(AssetManager* manager, StreamingAssetRefAny ref, stop_token stop) noexcept
        : _manager(manager), _ref(std::move(ref)), _stop(stop) {}

    bool await_ready() const noexcept {
        return _manager == nullptr || _stop.stop_requested() || !_ref.IsValid() || _ref.IsCompleted();
    }

    bool await_suspend(std::coroutine_handle<> continuation) {
        if (_manager == nullptr || _stop.stop_requested() || !_ref.IsValid() || _ref.IsCompleted()) {
            return false;
        }
        _record = _manager->RegisterWait(_ref.GetHandle(), _stop, continuation);
        return _record != nullptr;
    }

    bool await_resume() const noexcept {
        if (_record == nullptr) {
            return !_stop.stop_requested();
        }

        const bool completed = !_record->Canceled && !_record->Stop.stop_requested();
        if (_manager != nullptr) {
            _manager->_waiters.Erase(_record);
        }
        _record = nullptr;
        return completed;
    }

private:
    AssetManager* _manager;
    StreamingAssetRefAny _ref;
    stop_token _stop;
    mutable AssetWaitRecord* _record{nullptr};
};

// ════════════════════════════════════════════════════════════
//  StreamingAssetRefAny
// ════════════════════════════════════════════════════════════

namespace {

// 引用计数走共享控制块的原子变量,可在任意线程安全增减,不触碰 AssetManager / SparseSet。
void AddRefControl(const shared_ptr<AssetRefControl>& control) noexcept {
    if (control != nullptr) {
        control->RefCount.fetch_add(1, std::memory_order_relaxed);
    }
}

void ReleaseControl(const shared_ptr<AssetRefControl>& control) noexcept {
    if (control != nullptr) {
        control->RefCount.fetch_sub(1, std::memory_order_acq_rel);
    }
}

}  // namespace

StreamingAssetRefAny::StreamingAssetRefAny(
    AssetManager* manager,
    AssetHandle handle,
    AssetId id,
    shared_ptr<AssetRefControl> control) noexcept
    : _manager(manager), _handle(handle), _id(id), _control(std::move(control)) {
    AddRefControl(_control);
}

StreamingAssetRefAny::StreamingAssetRefAny(const StreamingAssetRefAny& other) noexcept
    : _manager(other._manager), _handle(other._handle), _id(other._id), _control(other._control) {
    AddRefControl(_control);
}

StreamingAssetRefAny::StreamingAssetRefAny(StreamingAssetRefAny&& other) noexcept
    : _manager(other._manager), _handle(other._handle), _id(other._id), _control(std::move(other._control)) {
    other._manager = nullptr;
    other._handle = AssetHandle::Invalid();
    other._id = AssetId{};
}

StreamingAssetRefAny& StreamingAssetRefAny::operator=(const StreamingAssetRefAny& other) noexcept {
    if (this == &other) {
        return *this;
    }
    AddRefControl(other._control);
    ReleaseControl(_control);
    _manager = other._manager;
    _handle = other._handle;
    _id = other._id;
    _control = other._control;
    return *this;
}

StreamingAssetRefAny& StreamingAssetRefAny::operator=(StreamingAssetRefAny&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    ReleaseControl(_control);
    _manager = other._manager;
    _handle = other._handle;
    _id = other._id;
    _control = std::move(other._control);
    other._manager = nullptr;
    other._handle = AssetHandle::Invalid();
    other._id = AssetId{};
    return *this;
}

StreamingAssetRefAny::~StreamingAssetRefAny() noexcept {
    ReleaseControl(_control);
}

void StreamingAssetRefAny::Reset() noexcept {
    ReleaseControl(_control);
    _control.reset();
    _manager = nullptr;
    _handle = AssetHandle::Invalid();
    _id = AssetId{};
}

Asset* StreamingAssetRefAny::Get() const noexcept {
    if (_manager == nullptr || !_handle.IsValid()) {
        return nullptr;
    }
    return _manager->ResolveAsset(_handle).Get();
}

bool StreamingAssetRefAny::IsValid() const noexcept {
    return _manager != nullptr && _manager->IsSlotAlive(_handle);
}

bool StreamingAssetRefAny::IsCompleted() const noexcept {
    if (_manager == nullptr) {
        return false;
    }
    AssetState s = _manager->ResolveState(_handle);
    return s != AssetState::Loading;
}

bool StreamingAssetRefAny::IsReady() const noexcept {
    return _manager != nullptr && _manager->ResolveState(_handle) == AssetState::Ready && Get() != nullptr;
}

bool StreamingAssetRefAny::IsFaulted() const noexcept {
    return _manager != nullptr && _manager->ResolveState(_handle) == AssetState::Faulted;
}

bool StreamingAssetRefAny::IsCanceled() const noexcept {
    return _manager != nullptr && _manager->ResolveState(_handle) == AssetState::Canceled;
}

void StreamingAssetRefAny::Cancel() const noexcept {
    if (_manager != nullptr) {
        _manager->Cancel(*this);
    }
}

AssetTypeId StreamingAssetRefAny::GetTypeId() const noexcept {
    if (_manager == nullptr || !_handle.IsValid()) {
        return Guid::Empty();
    }
    return _manager->ResolveTypeId(_handle);
}

AssetTypeId StreamingAssetRefAny::GetExpectedTypeId() const noexcept {
    if (_manager == nullptr || !_handle.IsValid()) {
        return Guid::Empty();
    }
    return _manager->ResolveExpectedTypeId(_handle);
}

// ════════════════════════════════════════════════════════════
//  AssetManager
// ════════════════════════════════════════════════════════════

AssetManager::~AssetManager() noexcept {
    for (auto& slot : _slots.Values()) {
        if (slot && slot->State == AssetState::Loading) {
            slot->Stop.request_stop();
        }
    }
    _loadScope.RequestStop();
    _loadScope.WaitUntilEmpty();
    Pump();

    for (auto& slot : _slots.Values()) {
        if (slot && slot->State == AssetState::Ready && slot->Object) {
            slot->Object->OnUnload(GetRecycler());
            slot->Object.reset();
        }
    }
}

AssetHandle AssetManager::FindHandle(const AssetId& id) const noexcept {
    auto it = _idIndex.find(id);
    if (it == _idIndex.end()) {
        return AssetHandle::Invalid();
    }
    return it->second;
}

AssetHandle AssetManager::EmplaceLoadingSlot(const AssetId& id, AssetTypeId typeId) {
    AssetHandle handle = _slots.Emplace(make_unique<AssetSlot>());
    AssetSlot* slot = _slots.Get(handle).get();
    slot->Id = id;
    slot->TypeId = typeId;
    slot->State = AssetState::Loading;
    _idIndex.emplace(id, handle);
    return handle;
}

StreamingAssetRefAny AssetManager::MakeRef(AssetHandle handle, const AssetId& id) noexcept {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    shared_ptr<AssetRefControl> control =
        (slotPtr != nullptr && *slotPtr != nullptr) ? slotPtr->get()->Control : nullptr;
    return StreamingAssetRefAny{this, handle, id, std::move(control)};
}

StreamingAssetRefAny AssetManager::Load(AssetLoadRequest request) {
    AssetHandle existing = FindHandle(request.Id);
    if (existing.IsValid()) {
        return MakeRef(existing, request.Id);
    }

    AssetHandle handle = EmplaceLoadingSlot(request.Id, request.ExpectedTypeId);
    _loadScope.Spawn(RunLoad(handle, std::move(request.Task)));
    _activeLoads.push_back(handle);

    return MakeRef(handle, request.Id);
}

task<void> AssetManager::Wait(StreamingAssetRefAny ref) {
    stop_token stop = co_await CurrentStopToken();
    bool completed = co_await AssetWaitAwaitable{this, std::move(ref), stop};
    if (!completed) {
        co_await StopCurrentTask();
    }
}

StreamingAssetRefAny AssetManager::AddReady(const AssetId& id, unique_ptr<Asset> object, AssetTypeId expectedTypeId) {
    AssetHandle existing = FindHandle(id);
    if (existing.IsValid()) {
        return MakeRef(existing, id);
    }
    AssetHandle handle = EmplaceLoadingSlot(id, expectedTypeId);
    OnLoadComplete(handle, AssetLoadResult::Success(std::move(object)));
    return MakeRef(handle, id);
}

task<void> AssetManager::RunLoad(AssetHandle handle, AssetLoadTask loadTask) {
    try {
        unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
        if (slotPtr == nullptr || *slotPtr == nullptr) {
            co_return;
        }

        stop_token stop = (*slotPtr)->Stop.get_token();
        std::optional<AssetLoadResult> result = co_await AwaitWithStopToken(std::move(loadTask), stop);
        if (!result.has_value()) {
            StoreLoadCanceled(handle);
            co_return;
        }
        StoreLoadResult(handle, std::move(result.value()));
    } catch (const std::exception& e) {
        StoreLoadResult(handle, AssetLoadResult::Failure(e.what()));
    } catch (...) {
        StoreLoadResult(handle, AssetLoadResult::Failure("unknown asset load exception"));
    }
}

void AssetManager::StoreLoadResult(AssetHandle handle, AssetLoadResult result) noexcept {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return;
    }
    AssetSlot* slot = slotPtr->get();
    slot->PendingResult = std::move(result);
}

void AssetManager::StoreLoadCanceled(AssetHandle handle) noexcept {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return;
    }
    AssetSlot* slot = slotPtr->get();
    slot->PendingCanceled = true;
}

StreamingAssetRefAny AssetManager::Find(const AssetId& id) noexcept {
    AssetHandle handle = FindHandle(id);
    if (!handle.IsValid()) {
        return StreamingAssetRefAny{};
    }
    return MakeRef(handle, id);
}

void AssetManager::Cancel(const StreamingAssetRefAny& ref) noexcept {
    AssetHandle handle = ref.GetHandle();
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return;
    }
    AssetSlot* slot = slotPtr->get();
    if (slot->State == AssetState::Loading) {
        slot->Stop.request_stop();
    }
}

void AssetManager::Unload(const AssetId& id) noexcept {
    AssetHandle handle = FindHandle(id);
    if (!handle.IsValid()) {
        return;
    }
#ifdef RADRAY_IS_DEBUG
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr != nullptr && *slotPtr != nullptr) {
        uint32_t refs = slotPtr->get()->Control->RefCount.load(std::memory_order_relaxed);
        if (refs > 0) {
            RADRAY_WARN_LOG(
                "AssetManager: force Unload asset {} with {} live reference(s); existing StreamingAssetRef will silently become invalid",
                id,
                refs);
        }
    }
#endif
    UnloadSlot(handle);
}

uint32_t AssetManager::CollectUnreferenced() noexcept {
    vector<AssetHandle> targets;
    const size_t slotCount = _slots.Count();
    std::span<const unique_ptr<AssetSlot>> values = _slots.Values();
    for (size_t i = 0; i < slotCount; ++i) {
        const AssetSlot* slot = values[i].get();
        if (slot == nullptr || slot->PendingUnload ||
            slot->Control->RefCount.load(std::memory_order_acquire) != 0) {
            continue;
        }
        AssetHandle handle = FindHandle(slot->Id);
        if (handle.IsValid()) {
            targets.push_back(handle);
        }
    }
    uint32_t collected = 0;
    for (AssetHandle handle : targets) {
        UnloadSlot(handle);
        ++collected;
    }
    return collected;
}

void AssetManager::UnloadSlot(AssetHandle handle) noexcept {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return;
    }
    AssetSlot* slot = slotPtr->get();
    if (slot->State == AssetState::Loading) {
        slot->PendingUnload = true;
        slot->Stop.request_stop();
        return;
    }
    if (slot->State == AssetState::Ready && slot->Object) {
        slot->Object->OnUnload(GetRecycler());
    }
    DestroySlot(handle);
}

void AssetManager::DestroySlot(AssetHandle handle) noexcept {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return;
    }
    AssetSlot* slot = slotPtr->get();
    _idIndex.erase(slot->Id);
    _slots.Destroy(handle);
}

void AssetManager::OnLoadComplete(AssetHandle handle, AssetLoadResult result) noexcept {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return;
    }
    AssetSlot* slot = slotPtr->get();
    if (!result.IsSuccess()) {
        if (!result.Error.empty()) {
            RADRAY_ERR_LOG("AssetManager: asset load failed: {}", result.Error);
        }
        slot->State = AssetState::Faulted;
        return;
    }
    unique_ptr<Asset> object = std::move(result.Object);
    if (!slot->TypeId.IsEmpty() && object->GetTypeId() != slot->TypeId) {
        RADRAY_ERR_LOG("AssetManager: loaded asset type does not match requested type");
        slot->State = AssetState::Faulted;
        return;
    }
    object->_id = slot->Id;
    slot->Object = std::move(object);
    slot->State = AssetState::Ready;
}

void AssetManager::OnLoadStopped(AssetHandle handle) noexcept {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return;
    }
    slotPtr->get()->State = AssetState::Canceled;
}

vector<AssetWaitRecord*> AssetManager::TakeWaiters(AssetHandle handle) noexcept {
    vector<AssetWaitRecord*> waiters;
    const size_t waiterCount = _waiters.Count();
    for (size_t i = 0; i < waiterCount; ++i) {
        AssetWaitRecord* waiter = _waiters.At(i);
        if (waiter != nullptr && waiter->Handle == handle) {
            waiters.push_back(waiter);
        }
    }
    return waiters;
}

void AssetManager::ResumeWaiters(std::span<AssetWaitRecord* const> waiters) noexcept {
    for (AssetWaitRecord* waiter : waiters) {
        if (waiter == nullptr) {
            continue;
        }
        if (!_waiters.IsAlive(waiter)) {
            continue;
        }
        _waiters.ResumeRecord(waiter);
    }
}

void AssetManager::FinalizeTerminalSlot(AssetHandle handle) {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return;
    }
    AssetSlot* slot = slotPtr->get();
    if (slot->PendingUnload) {
        if (slot->State == AssetState::Ready && slot->Object) {
            slot->Object->OnUnload(GetRecycler());
        }
        DestroySlot(handle);
    }
}

AssetWaitRecord* AssetManager::RegisterWait(
    AssetHandle handle,
    stop_token stop,
    std::coroutine_handle<> continuation) {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr || *slotPtr == nullptr || (*slotPtr)->State != AssetState::Loading) {
        return nullptr;
    }

    AssetWaitRecord* record = _waiters.Enqueue(stop, continuation);
    record->Handle = handle;
    return record;
}

IRenderResourceRecycler& AssetManager::GetRecycler() noexcept {
    if (_recycler != nullptr) {
        return *_recycler;
    }
    static ImmediateRenderResourceRecycler immediate;
    return immediate;
}

void AssetManager::Pump() {
    for (size_t i = 0; i < _activeLoads.size();) {
        AssetHandle handle = _activeLoads[i];
        unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
        if (slotPtr == nullptr || *slotPtr == nullptr) {
            _activeLoads.erase(_activeLoads.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }

        AssetSlot* slot = slotPtr->get();
        if (!slot->PendingCanceled && !slot->PendingResult.has_value()) {
            ++i;
            continue;
        }

        if (slot->PendingCanceled) {
            OnLoadStopped(handle);
            slot->PendingCanceled = false;
        } else if (slot->PendingResult.has_value()) {
            OnLoadComplete(handle, std::move(slot->PendingResult.value()));
            slot->PendingResult.reset();
        }
        vector<AssetWaitRecord*> waiters = TakeWaiters(handle);
        FinalizeTerminalSlot(handle);
        ResumeWaiters(waiters);

        _activeLoads.erase(_activeLoads.begin() + static_cast<ptrdiff_t>(i));
    }
}

Nullable<Asset*> AssetManager::ResolveAsset(AssetHandle handle) const noexcept {
    const unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return nullptr;
    }
    const AssetSlot* slot = slotPtr->get();
    if (slot->State != AssetState::Ready || !slot->Object) {
        return nullptr;
    }
    return slot->Object.get();
}

AssetTypeId AssetManager::ResolveTypeId(AssetHandle handle) const noexcept {
    const unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return Guid::Empty();
    }
    const AssetSlot* slot = slotPtr->get();
    if (slot->State != AssetState::Ready || !slot->Object) {
        return Guid::Empty();
    }
    return slot->Object->GetTypeId();
}

AssetTypeId AssetManager::ResolveExpectedTypeId(AssetHandle handle) const noexcept {
    const unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return Guid::Empty();
    }
    return slotPtr->get()->TypeId;
}

AssetState AssetManager::ResolveState(AssetHandle handle) const noexcept {
    const unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return AssetState::Faulted;
    }
    return slotPtr->get()->State;
}

bool AssetManager::IsSlotAlive(AssetHandle handle) const noexcept {
    return _slots.TryGet(handle) != nullptr;
}

}  // namespace radray
