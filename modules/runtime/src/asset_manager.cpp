#include <radray/runtime/asset_manager.h>

#include <radray/logger.h>
#include <radray/runtime/wait_frame.h>

namespace radray {

void ServiceTraits<AssetManager>::Inject(AssetManager& self, IWaitFrameProcessor& frames, Nullable<IAssetSource*> source) noexcept {
    self.SetWaitFrameProcessor(&frames);
    self.SetAssetSource(source);
}

void ServiceTraits<AssetManager>::Unwire(AssetManager& self) noexcept {
    self.SetAssetSource(nullptr);
    self.SetWaitFrameProcessor(nullptr);
}

/// 一个资产的槽位。地址稳定 (unordered_map 里的 unique_ptr 元素), 故 StreamingAssetRefAny
/// 直接持它的裸指针 —— RefCount > 0 期间它一定不被销毁。
struct AssetSlot {
    AssetId Id;
    AssetState State{AssetState::Loading};
    unique_ptr<Asset> Object;
    stop_source Stop;
    /// 加载协程写入、Pump 提交。协程与 Pump 都在主线程, 故无需同步。
    std::optional<AssetLoadResult> PendingResult;
    bool PendingCanceled{false};
    /// 【普通整数, 非原子】: 引用只在主线程增减 (见 StreamingAssetRefAny 的线程说明)。
    uint32_t RefCount{0};
};

using Slot = AssetSlot;

bool AssetWaitAwaitable::Suspend(std::coroutine_handle<> continuation, stop_token stop) {
    AssetManager* manager = _ref._manager;
    if (manager == nullptr || _ref._slot == nullptr || _ref.IsCompleted()) {
        return false;
    }
    if (stop.stop_requested()) {
        // 【已被取消则不挂起】: 挂上去也只会等到下一次泵才被摘除, 而结论此刻就已确定。
        // 记下来供 await_resume 区分 "没挂起因为已是终态" 与 "没挂起因为已被取消"。
        _canceledBeforeSuspend = true;
        return false;
    }
    _record = manager->RegisterWait(_ref._slot, stop, continuation);
    return _record != nullptr;
}

bool AssetWaitAwaitable::await_resume() noexcept {
    if (_record == nullptr) {
        // 没挂起过: await_ready 为真 (空引用或已是终态) 或 Suspend 拒绝。
        return !_canceledBeforeSuspend;
    }
    const bool completed = !_record->Canceled && !_record->Stop.stop_requested();
    if (AssetManager* manager = _ref._manager; manager != nullptr) {
        manager->_waiters.Erase(_record);
    }
    _record = nullptr;
    return completed;
}

// ════════════════════════════════════════════════════════════
//  StreamingAssetRefAny
// ════════════════════════════════════════════════════════════

StreamingAssetRefAny::StreamingAssetRefAny(AssetManager* manager, Slot* slot) noexcept
    : _manager(manager), _slot(slot) {
    AssetManager::AddRef(_slot);
}

StreamingAssetRefAny::StreamingAssetRefAny(const StreamingAssetRefAny& other) noexcept
    : _manager(other._manager), _slot(other._slot) {
    AssetManager::AddRef(_slot);
}

StreamingAssetRefAny::StreamingAssetRefAny(StreamingAssetRefAny&& other) noexcept
    : _manager(other._manager), _slot(other._slot) {
    other._manager = nullptr;
    other._slot = nullptr;
}

StreamingAssetRefAny& StreamingAssetRefAny::operator=(const StreamingAssetRefAny& other) noexcept {
    if (this == &other) {
        return *this;
    }
    // 先加后减: other 与 *this 可能指向同一个 slot, 反过来会让计数瞬间归零。
    AssetManager::AddRef(other._slot);
    AssetManager::Release(_slot);
    _manager = other._manager;
    _slot = other._slot;
    return *this;
}

StreamingAssetRefAny& StreamingAssetRefAny::operator=(StreamingAssetRefAny&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    AssetManager::Release(_slot);
    _manager = other._manager;
    _slot = other._slot;
    other._manager = nullptr;
    other._slot = nullptr;
    return *this;
}

StreamingAssetRefAny::~StreamingAssetRefAny() noexcept {
    AssetManager::Release(_slot);
}

void StreamingAssetRefAny::Reset() noexcept {
    AssetManager::Release(_slot);
    _manager = nullptr;
    _slot = nullptr;
}

Nullable<Asset*> StreamingAssetRefAny::Get() const noexcept {
    if (_slot == nullptr || _slot->State != AssetState::Ready) {
        return nullptr;
    }
    return _slot->Object.get();
}

bool StreamingAssetRefAny::IsValid() const noexcept {
    return _slot != nullptr;
}

bool StreamingAssetRefAny::IsCompleted() const noexcept {
    if (_slot == nullptr) {
        return false;
    }
    const AssetState state = _slot->State;
    return state == AssetState::Ready || state == AssetState::Faulted || state == AssetState::Canceled;
}

bool StreamingAssetRefAny::IsReady() const noexcept {
    return _slot != nullptr && _slot->State == AssetState::Ready;
}

bool StreamingAssetRefAny::IsFaulted() const noexcept {
    return _slot != nullptr && _slot->State == AssetState::Faulted;
}

bool StreamingAssetRefAny::IsCanceled() const noexcept {
    return _slot != nullptr && _slot->State == AssetState::Canceled;
}

void StreamingAssetRefAny::Cancel() const noexcept {
    if (_manager != nullptr) {
        _manager->Cancel(*this);
    }
}

const AssetId& StreamingAssetRefAny::GetAssetId() const noexcept {
    static const AssetId empty{};
    return _slot != nullptr ? _slot->Id : empty;
}

// ════════════════════════════════════════════════════════════
//  AssetManager
// ════════════════════════════════════════════════════════════

AssetManager::AssetManager() noexcept = default;

AssetManager::~AssetManager() noexcept {
    // 1. 停掉在飞加载并等协程退出。
    for (auto& [id, slot] : _slots) {
        if (slot && slot->State == AssetState::Loading) {
            slot->Stop.request_stop();
        }
    }
    _loadScope.RequestStop();
    _loadScope.WaitUntilEmpty();

    // 2. 提交残留结果, 再放开 _activeLoads 里那些引用, 然后回收已归零的资产。
    //
    //    【顺序不能反】PumpLoadResults 遍历的正是 _activeLoads —— 先 clear 会让"协程已写好
    //    结果但还没 Pump"的资产随 optional 析构而永不走 OnUnload, GPU 对象活过 device。
    //    【刻意不调 Pump】它会 spawn 一个立刻被取消的协程去等帧边界; 这里直接同步走完。
    PumpLoadResults();
    _activeLoads.clear();
    CollectZeroRefSlots();

    // 3. 无条件对残留槽位走一遍 OnUnload + 析构, 即便仍被引用。
    //
    //    此刻还有引用是关停顺序的使用错误。
    //    两种坏结果里选销毁: GPU 资源必须在 device 之前交出, 泄漏比悬垂更难查。
    //    不 abort 是因为关停期 abort 会掩盖真正的首因。
    for (auto& [id, slot] : _slots) {
        if (slot && slot->RefCount > 0) {
            RADRAY_ERR_LOG(
                "AssetManager: asset {} still has {} live reference(s) at shutdown; unloading anyway (those references are now dangling) — release all StreamingAssetRef before destroying AssetManager, see shutdown order in Application::Shutdown",
                id,
                slot->RefCount);
        }
        if (slot && slot->Object) {
            slot->Object->OnUnload(*this);
            slot->Object.reset();
        }
    }
    _slots.clear();

    // 4. 刚才 OnUnload 交出的 payload 已无从等待帧边界 (_loadScope 已停)。就地销毁。
    //    【为何安全】: 关停路径在此之前已经 device wait-idle 过 (Application::Shutdown 先
    //    调 GpuSystem::WaitAndCleanupCompletedFlights), 故 GPU 上没有仍在读这些对象的 work。
    _pendingDeferred.clear();
}

Slot* AssetManager::FindSlot(const AssetId& id) const noexcept {
    auto it = _slots.find(id);
    return it == _slots.end() ? nullptr : it->second.get();
}

Slot* AssetManager::EmplaceLoadingSlot(const AssetId& id) {
    auto slot = make_unique<Slot>();
    slot->Id = id;
    slot->State = AssetState::Loading;
    Slot* raw = slot.get();
    _slots.emplace(id, std::move(slot));
    return raw;
}

StreamingAssetRefAny AssetManager::MakeRef(Slot* slot) noexcept {
    return StreamingAssetRefAny{this, slot};
}

void AssetManager::AddRef(Slot* slot) noexcept {
    if (slot != nullptr) {
        ++slot->RefCount;
    }
}

void AssetManager::Release(Slot* slot) noexcept {
    if (slot != nullptr) {
        --slot->RefCount;
    }
    // 归零【不】在此销毁。析构路径是 noexcept 且可能正处在资产表的遍历中,
    // 就地销毁会递归跑资产析构并使迭代器失效 —— 理由详见头文件 AssetManager 的说明。
    // 实际销毁由 Pump -> CollectZeroRefSlots 完成。
}

StreamingAssetRefAny AssetManager::Load(AssetLoadRequest request) {
    if (Slot* existing = FindSlot(request.Id); existing != nullptr) {
        return MakeRef(existing);
    }

    Slot* slot = EmplaceLoadingSlot(request.Id);
    StreamingAssetRefAny ref = MakeRef(slot);
    // 【manager 自持一份引用直到加载跑完】: 外部引用可能在加载途中全部消失, 但我们不
    // request_stop —— 加载多半已花掉大半代价 (IO 已完成、GPU 上传已提交), 半途取消既救不回
    // 那部分开销, 又要在每个 loader 里写"取消后如何回退"。让它跑完, 之后按常规归零回收。
    _activeLoads.push_back(ref);
    _loadScope.Spawn(RunLoad(ref, std::move(request.Task)));

    return ref;
}

StreamingAssetRefAny AssetManager::Load(const AssetId& id) {
    if (Slot* existing = FindSlot(id); existing != nullptr) {
        return MakeRef(existing);
    }
    if (!_assetSource.HasValue()) {
        RADRAY_ERR_LOG("AssetManager: no asset source is installed for asset {}", id);
        return {};
    }

    std::optional<task<AssetLoadResult>> loadTask = _assetSource->CreateLoadTask(id);
    if (!loadTask.has_value()) {
        RADRAY_ERR_LOG("AssetManager: asset source cannot load asset {}", id);
        return {};
    }
    return Load(AssetLoadRequest{
        .Id = id,
        .Task = std::move(loadTask.value()),
        .DebugName = id.ToString()});
}

StreamingAssetRefAny AssetManager::LoadSourcePath(std::string_view relPath) {
    if (!_assetSource.HasValue()) {
        RADRAY_ERR_LOG("AssetManager: no asset source is installed for path '{}'", relPath);
        return {};
    }
    const std::optional<AssetId> id = _assetSource->ResolveId(relPath);
    if (!id.has_value()) {
        RADRAY_ERR_LOG("AssetManager: asset source cannot resolve path '{}'", relPath);
        return {};
    }
    return Load(id.value());
}

task<void> AssetManager::Wait(StreamingAssetRefAny ref) {
    const bool completed = co_await ref;
    if (!completed) {
        co_await StopCurrentTask();
    }
}

StreamingAssetRefAny AssetManager::AddReady(
    const AssetId& id,
    unique_ptr<Asset> object) {
    if (Slot* existing = FindSlot(id); existing != nullptr) {
        return MakeRef(existing);
    }
    Slot* slot = EmplaceLoadingSlot(id);
    // 先建引用再提交结果: CommitLoadResult 不会销毁槽位, 但保持"槽位一旦存在就有人持有"
    // 这条不变量能让后续改动不必再推理中间态。
    StreamingAssetRefAny ref = MakeRef(slot);
    CommitLoadResult(slot, AssetLoadResult::Success(std::move(object)));
    return ref;
}

task<void> AssetManager::RunLoad(StreamingAssetRefAny ref, task<AssetLoadResult> loadTask) {
    Slot* slot = ref._slot;
    if (slot == nullptr) {
        co_return;
    }
    // 【不捕获异常】: loader 抛出的异常一律传播。从 loader 里逃出来的异常不是"加载失败"
    // 这类预期结果 (那条路由 AssetLoadResult::Failure 表达), 而是分配失败或不变量被破坏,
    // 把它转成 Faulted 只会掩盖首因。见 AGENTS.md 的异常策略。
    stop_token stop = slot->Stop.get_token();
    std::optional<AssetLoadResult> result = co_await AwaitWithStopToken(std::move(loadTask), stop);
    if (!result.has_value()) {
        StoreLoadCanceled(slot);
        co_return;
    }
    StoreLoadResult(slot, std::move(result.value()));
}

void AssetManager::StoreLoadResult(Slot* slot, AssetLoadResult result) noexcept {
    slot->PendingResult = std::move(result);
}

void AssetManager::StoreLoadCanceled(Slot* slot) noexcept {
    slot->PendingCanceled = true;
}

StreamingAssetRefAny AssetManager::Find(const AssetId& id) noexcept {
    Slot* slot = FindSlot(id);
    if (slot == nullptr) {
        return StreamingAssetRefAny{};
    }
    return MakeRef(slot);
}

void AssetManager::Cancel(const StreamingAssetRefAny& ref) noexcept {
    Slot* slot = ref._slot;
    if (slot != nullptr && slot->State == AssetState::Loading) {
        slot->Stop.request_stop();
    }
}

void AssetManager::CommitLoadResult(Slot* slot, AssetLoadResult result) noexcept {
    if (!result.IsSuccess()) {
        if (!result.Error.empty()) {
            RADRAY_ERR_LOG("AssetManager: asset load failed: {}", result.Error);
        }
        slot->State = AssetState::Faulted;
        return;
    }
    unique_ptr<Asset> object = std::move(result.Object);
    object->_id = slot->Id;
    slot->Object = std::move(object);
    slot->State = AssetState::Ready;
}

void AssetManager::ResumeWaiters(Slot* slot) noexcept {
    // 先收集再恢复: 恢复会让等待者从 _waiters 里摘掉自己的记录, 边遍历边恢复会失效。
    vector<AssetWaitRecord*> targets;
    const size_t count = _waiters.Count();
    for (size_t i = 0; i < count; ++i) {
        AssetWaitRecord* waiter = _waiters.At(i);
        if (waiter != nullptr && waiter->Slot == slot) {
            targets.push_back(waiter);
        }
    }
    for (AssetWaitRecord* waiter : targets) {
        if (_waiters.IsAlive(waiter)) {
            _waiters.ResumeRecord(waiter);
        }
    }
}

AssetWaitRecord* AssetManager::RegisterWait(
    Slot* slot,
    stop_token stop,
    std::coroutine_handle<> continuation) {
    if (slot == nullptr || slot->State != AssetState::Loading) {
        return nullptr;
    }
    AssetWaitRecord* record = _waiters.Enqueue(stop, continuation);
    record->Slot = slot;
    return record;
}

void AssetManager::DestroySlot(Slot* slot) noexcept {
    // Object 先析构再摘表: 资产析构可能查询 manager (例如放开它自己持有的引用),
    // 此时表里还留着自己的槽位是无害的, 而反过来则会让 unique_ptr 析构发生在
    // erase 内部、此时 slot 指针已不可用。
    slot->Object.reset();
    _slots.erase(slot->Id);
}

void AssetManager::CollectZeroRefSlots() {
    if (_collecting) {
        return;
    }
    _collecting = true;

    // 循环到不动点: 销毁一个资产会放开它持有的 StreamingAssetRef, 从而可能令别的
    // 槽位归零。每轮重新扫表, 因为上一轮的销毁已经改过 _slots。
    for (;;) {
        vector<Slot*> zeroRef;
        for (auto& [id, slot] : _slots) {
            if (slot && slot->RefCount == 0) {
                zeroRef.push_back(slot.get());
            }
        }
        if (zeroRef.empty()) {
            break;
        }
        for (Slot* slot : zeroRef) {
            // 上一轮的销毁不会令这里的指针失效 (只有本循环销毁槽位, 且每个只销毁一次),
            // 但资产析构可能新建引用又放开, 故仍要复查 RefCount。
            if (slot->RefCount > 0) {
                continue;
            }
            if (slot->State == AssetState::Ready && slot->Object) {
                slot->Object->OnUnload(*this);
            }
            DestroySlot(slot);
        }
    }

    _collecting = false;
}

void AssetManager::EnqueueDeferred(unique_ptr<DeferredPayload> payload) {
    if (payload == nullptr) {
        return;
    }
    if (_waitFrame == nullptr) {
        // 【为何是 error log + 立即销毁, 而不是 abort】: 走到这里时 payload 已经被移交,
        // 唯一的替代动作是泄漏。而漏装配 wait processor 在【纯 CPU 资产】的场景下并不导致
        // 错误 —— 那类资产的 OnUnload 交出的东西本就可以立即销毁 (测试用的 AssetManager
        // 便不装配)。故这里不 abort, 但把它记成 error: 若交出的是 GPU 对象, 立即销毁就是
        // 绕过 fence 等待, 必须被看见。
        RADRAY_ERR_LOG("AssetManager: wait frame processor not wired; destroying deferred payload immediately (see SetWaitFrameProcessor)");
        payload.reset();
        return;
    }
    _pendingDeferred.push_back(std::move(payload));
}

task<void> AssetManager::RunDeferredDestroy(vector<unique_ptr<DeferredPayload>> batch) {
    co_await _waitFrame->Wait();
    // 恢复点在主线程 (见 IWaitFrameProcessor 的恢复线程约定), 故这里析构 GPU 对象是安全的。
    // 取消时协程帧连同 batch 一起销毁, 效果相同 —— 不需要额外的兜底分支。
    batch.clear();
}

void AssetManager::PumpLoadResults() {
    for (size_t i = 0; i < _activeLoads.size();) {
        Slot* slot = _activeLoads[i]._slot;
        if (slot == nullptr) {
            _activeLoads.erase(_activeLoads.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }
        if (!slot->PendingCanceled && !slot->PendingResult.has_value()) {
            ++i;
            continue;
        }

        if (slot->PendingCanceled) {
            slot->State = AssetState::Canceled;
            slot->PendingCanceled = false;
        } else {
            CommitLoadResult(slot, std::move(slot->PendingResult.value()));
            slot->PendingResult.reset();
        }
        ResumeWaiters(slot);

        // 放开 manager 自持的那份引用。可能就此归零, 由 CollectZeroRefSlots 处理。
        _activeLoads.erase(_activeLoads.begin() + static_cast<ptrdiff_t>(i));
    }
}

void AssetManager::FlushDeferredBatch() {
    // 【一帧一个协程帧】: 本帧攒下的 payload 整批交给一个等待协程, 而不是每个 payload 一个。
    if (_pendingDeferred.empty()) {
        return;
    }
    vector<unique_ptr<DeferredPayload>> batch = std::move(_pendingDeferred);
    _pendingDeferred.clear();
    _loadScope.Spawn(RunDeferredDestroy(std::move(batch)));
}

void AssetManager::Pump() {
    PumpLoadResults();
    CollectZeroRefSlots();
    FlushDeferredBatch();
}

uint32_t AssetManager::GetAssetCount() const noexcept {
    return static_cast<uint32_t>(_slots.size());
}

}  // namespace radray
