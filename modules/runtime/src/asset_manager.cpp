#include <radray/runtime/asset_manager.h>

#include <exception>

#include <radray/logger.h>

namespace radray {

// ════════════════════════════════════════════════════════════
//  StreamingAssetRefAny
// ════════════════════════════════════════════════════════════

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
            slot->Object->OnUnload();
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

StreamingAssetRefAny AssetManager::Load(AssetLoadRequest request) {
    AssetHandle existing = FindHandle(request.Id);
    if (existing.IsValid()) {
        return StreamingAssetRefAny{this, existing, request.Id};
    }

    AssetHandle handle = EmplaceLoadingSlot(request.Id, request.ExpectedTypeId);
    _loadScope.Spawn(RunLoad(handle, std::move(request.Task)));
    _activeLoads.push_back(handle);

    return StreamingAssetRefAny{this, handle, request.Id};
}

StreamingAssetRefAny AssetManager::AddReady(const AssetId& id, unique_ptr<Asset> object, AssetTypeId expectedTypeId) {
    AssetHandle existing = FindHandle(id);
    if (existing.IsValid()) {
        return StreamingAssetRefAny{this, existing, id};
    }
    AssetHandle handle = EmplaceLoadingSlot(id, expectedTypeId);
    OnLoadComplete(handle, AssetLoadResult::Success(std::move(object)));
    return StreamingAssetRefAny{this, handle, id};
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
    return StreamingAssetRefAny{this, handle, id};
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
        slot->Object->OnUnload();
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

void AssetManager::FinalizeTerminalSlot(AssetHandle handle) {
    unique_ptr<AssetSlot>* slotPtr = _slots.TryGet(handle);
    if (slotPtr == nullptr) {
        return;
    }
    AssetSlot* slot = slotPtr->get();
    if (slot->PendingUnload) {
        if (slot->State == AssetState::Ready && slot->Object) {
            slot->Object->OnUnload();
        }
        DestroySlot(handle);
    }
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
        FinalizeTerminalSlot(handle);

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
