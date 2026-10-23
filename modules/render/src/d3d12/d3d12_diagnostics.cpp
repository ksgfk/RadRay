#include "d3d12_diagnostics.h"

#include <algorithm>
#include <atomic>
#include <mutex>

// 诊断契约见 docs/architecture/render-rhi.md。
namespace radray::render::d3d12::detail {

static LogLevel _MapD3D12ValidationLogLevel(D3D12_MESSAGE_SEVERITY severity) noexcept {
    switch (severity) {
        case D3D12_MESSAGE_SEVERITY_CORRUPTION: return LogLevel::Critical;
        case D3D12_MESSAGE_SEVERITY_ERROR: return LogLevel::Err;
        case D3D12_MESSAGE_SEVERITY_WARNING: return LogLevel::Warn;
        case D3D12_MESSAGE_SEVERITY_INFO: return LogLevel::Info;
        case D3D12_MESSAGE_SEVERITY_MESSAGE: return LogLevel::Debug;
        default: return LogLevel::Info;
    }
}

struct DiagnosticReceiver {
    std::mutex Mutex;
    bool Enabled{true};
    size_t Sessions{0};
    shared_ptr<DiagnosticObserver> Observer;
    Nullable<RenderLogCallback> Callback;
    Nullable<void*> UserData;
};

struct DiagnosticState;

class DiagnosticContext final : public DXGIDiagnosticContext {
public:
    // Native callbacks take Mutex/receiver locks only; topology and readers
    // serialize on CollectionMutex, including nested failure checkpoints.
    std::mutex Mutex;
    std::recursive_mutex CollectionMutex;
    uint64_t NextId{1};
    vector<shared_ptr<DiagnosticState>> Channels;
    vector<weak_ptr<DiagnosticReceiver>> Observers;
    vector<weak_ptr<DiagnosticReceiver>> Loggers;
};

static DiagnosticContext& _Context(const shared_ptr<DXGIDiagnosticContext>& context) noexcept {
    return static_cast<DiagnosticContext&>(*context);
}

struct DiagnosticState {
    explicit DiagnosticState(DiagnosticContext& context) noexcept : Context(context) {}
    // Sessions keep the context alive until the last channel has unregistered.
    DiagnosticContext& Context;
    DiagnosticSource Source{DiagnosticSource::D3D12};
    DiagnosticMode Mode{DiagnosticMode::Polling};
    uint64_t Id{0};
    GUID Producer{};
    size_t Users{0};
    ComPtr<IUnknown> Identity;
    ComPtr<ID3D12InfoQueue> D3D12;
    ComPtr<ID3D12InfoQueue1> CallbackQueue;
    ComPtr<IDXGIInfoQueue> Dxgi;
    vector<std::pair<shared_ptr<DiagnosticReceiver>, size_t>> Receivers;
    std::atomic<bool> Healthy{true};
    uint32_t ReportedFailures{0};
    array<HRESULT, static_cast<size_t>(DiagnosticFailure::ShutdownFailed) + 1> FailureResults{};
    bool Closed{false};
    DiagnosticQueueSnapshot Snapshot;
    UINT64 OldLimit{0};
    UINT64 ExpectedLimit{65536};
    UINT StorageDepth{0}, RetrievalDepth{0};
    bool StoragePushed{false}, RetrievalPushed{false}, LimitSet{false};
    std::optional<DWORD> Cookie;
};

static void _DispatchDiagnostic(DiagnosticState& channel, const DiagnosticMessage& message) noexcept {
    vector<shared_ptr<DiagnosticReceiver>> receivers;
    {
        auto& registry = channel.Context;
        std::lock_guard lock{registry.Mutex};
        for (const auto& [receiver, users] : channel.Receivers) {
            if (users != 0) receivers.push_back(receiver);
        }
        for (const auto& observer : registry.Observers) {
            if (auto receiver = observer.lock()) receivers.push_back(std::move(receiver));
        }
    }
    const auto level = _MapD3D12ValidationLogLevel(static_cast<D3D12_MESSAGE_SEVERITY>(message.Severity));
    const auto text = fmt::format(
        "{} {} channel:{} producer:{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x} category:{} id:{} HRESULT:{:#x}\n{}",
        message.Source == DiagnosticSource::D3D12 ? "d3d12" : "dxgi",
        message.IsCollectionFailure() ? "diagnostic collection failure" : "validation message",
        message.Channel, message.Producer.Data1, message.Producer.Data2, message.Producer.Data3,
        message.Producer.Data4[0], message.Producer.Data4[1], message.Producer.Data4[2], message.Producer.Data4[3],
        message.Producer.Data4[4], message.Producer.Data4[5], message.Producer.Data4[6], message.Producer.Data4[7],
        message.Category, message.Id, static_cast<uint32_t>(message.Result), message.Description);
    bool logged = false;
    for (const auto& receiver : receivers) {
        std::lock_guard lock{receiver->Mutex};
        if (!receiver->Enabled) continue;
        if (receiver->Observer) receiver->Observer->OnDiagnostic(message);
        if (receiver->Callback) {
            receiver->Callback.Get()(level, text, receiver->UserData.Get());
            logged = true;
        }
    }
    if (!logged) {
        switch (level) {
            case LogLevel::Critical:
            case LogLevel::Err: RADRAY_ERR_LOG("{}", text); break;
            case LogLevel::Warn: RADRAY_WARN_LOG("{}", text); break;
            case LogLevel::Info: RADRAY_INFO_LOG("{}", text); break;
            default: RADRAY_DEBUG_LOG("{}", text); break;
        }
    }
}

static DiagnosticMessage _CollectionFailure(DiagnosticState& channel, DiagnosticFailure failure,
                                            std::string_view reason, HRESULT result) {
    channel.Healthy = false;
    channel.ReportedFailures |= 1u << static_cast<uint32_t>(failure);
    channel.FailureResults[static_cast<size_t>(failure)] = result;
    return {.Source = channel.Source, .Channel = channel.Id, .Producer = channel.Producer, .Severity = D3D12_MESSAGE_SEVERITY_ERROR, .Description = string{reason}, .Result = result, .Failure = failure};
}

static void WINAPI _DiagnosticCallback(D3D12_MESSAGE_CATEGORY category, D3D12_MESSAGE_SEVERITY severity,
                                       D3D12_MESSAGE_ID id, LPCSTR description, void* context) noexcept {
    auto& channel = *static_cast<DiagnosticState*>(context);
    _DispatchDiagnostic(channel, {.Source = DiagnosticSource::D3D12, .Channel = channel.Id, .Severity = static_cast<uint32_t>(severity), .Category = static_cast<uint32_t>(category), .Id = static_cast<int32_t>(id), .Description = description == nullptr ? "" : description});
}

template <class Filter>
static bool _EmptyDiagnosticFilter(const Filter& filter) noexcept {
    return filter.AllowList.NumCategories == 0 && filter.AllowList.NumSeverities == 0 && filter.AllowList.NumIDs == 0 &&
           filter.DenyList.NumCategories == 0 && filter.DenyList.NumSeverities == 0 && filter.DenyList.NumIDs == 0;
}

static bool _DiagnosticFiltersIntact(DiagnosticState& channel) noexcept {
    if (channel.D3D12) {
        D3D12_INFO_QUEUE_FILTER storage{}, retrieval{};
        SIZE_T storageSize = sizeof(storage), retrievalSize = sizeof(retrieval);
        return channel.D3D12->GetStorageFilterStackSize() == channel.StorageDepth &&
               channel.D3D12->GetRetrievalFilterStackSize() == channel.RetrievalDepth &&
               SUCCEEDED(channel.D3D12->GetStorageFilter(&storage, &storageSize)) && storageSize == sizeof(storage) &&
               SUCCEEDED(channel.D3D12->GetRetrievalFilter(&retrieval, &retrievalSize)) && retrievalSize == sizeof(retrieval) &&
               _EmptyDiagnosticFilter(storage) && _EmptyDiagnosticFilter(retrieval);
    }
    DXGI_INFO_QUEUE_FILTER storage{}, retrieval{};
    SIZE_T storageSize = sizeof(storage), retrievalSize = sizeof(retrieval);
    return channel.Dxgi->GetStorageFilterStackSize(channel.Producer) == channel.StorageDepth &&
           channel.Dxgi->GetRetrievalFilterStackSize(channel.Producer) == channel.RetrievalDepth &&
           SUCCEEDED(channel.Dxgi->GetStorageFilter(channel.Producer, &storage, &storageSize)) && storageSize == sizeof(storage) &&
           SUCCEEDED(channel.Dxgi->GetRetrievalFilter(channel.Producer, &retrieval, &retrievalSize)) && retrievalSize == sizeof(retrieval) &&
           _EmptyDiagnosticFilter(storage) && _EmptyDiagnosticFilter(retrieval);
}

template <class Message, class Read>
static HRESULT _ReadDiagnosticMessage(Read read, UINT64 index, vector<byte>& scratch, DiagnosticMessage& result) noexcept {
    SIZE_T size = 0;
    HRESULT status = read(index, nullptr, &size);
    if (FAILED(status)) return status;
    if (size < sizeof(Message) || size > 1024 * 1024) return E_UNEXPECTED;
    scratch.resize(size);
    auto* message = reinterpret_cast<Message*>(scratch.data());
    status = read(index, message, &size);
    if (FAILED(status)) return status;
    if (size < sizeof(Message) || size > scratch.size()) return E_UNEXPECTED;
    const auto begin = reinterpret_cast<uintptr_t>(scratch.data());
    const auto description = reinterpret_cast<uintptr_t>(message->pDescription);
    const auto length = message->DescriptionByteLength;
    if (description < begin + sizeof(Message) || description > begin + size ||
        length == 0 || length > begin + size - description || message->pDescription[length - 1] != '\0') return E_UNEXPECTED;
    result.Severity = static_cast<uint32_t>(message->Severity);
    result.Category = static_cast<uint32_t>(message->Category);
    result.Id = static_cast<int32_t>(message->ID);
    result.Description.assign(message->pDescription, length - 1);
    if constexpr (std::is_same_v<Message, DXGI_INFO_QUEUE_MESSAGE>) result.Producer = message->Producer;
    return S_OK;
}

template <class Read>
static HRESULT _ReadDiagnosticSnapshot(Read read, DiagnosticQueueSnapshot& snapshot) noexcept {
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        const auto before = read();
        const auto after = read();
        if (before != after) continue;
        if (after.Stored > after.Accepted || after.Discarded > after.Accepted - after.Stored) return E_UNEXPECTED;
        snapshot = after;
        return S_OK;
    }
    return E_PENDING;
}

HRESULT ReadDiagnosticSnapshot(ID3D12InfoQueue& queue, DiagnosticQueueSnapshot& snapshot) noexcept {
    const auto status = _ReadDiagnosticSnapshot([&] {
        return DiagnosticQueueSnapshot{queue.GetNumMessagesAllowedByStorageFilter(), queue.GetNumStoredMessages(),
                                       queue.GetNumMessagesDiscardedByMessageCountLimit(), queue.GetNumMessagesDeniedByStorageFilter()};
    },
                                                snapshot);
    if (SUCCEEDED(status) && queue.GetNumStoredMessagesAllowedByRetrievalFilter() != snapshot.Stored) return E_UNEXPECTED;
    return status;
}

HRESULT ReadDiagnosticSnapshot(IDXGIInfoQueue& queue, GUID producer, DiagnosticQueueSnapshot& snapshot) noexcept {
    const auto status = _ReadDiagnosticSnapshot([&] {
        return DiagnosticQueueSnapshot{queue.GetNumMessagesAllowedByStorageFilter(producer), queue.GetNumStoredMessages(producer),
                                       queue.GetNumMessagesDiscardedByMessageCountLimit(producer), queue.GetNumMessagesDeniedByStorageFilter(producer)};
    },
                                                snapshot);
    if (SUCCEEDED(status) && queue.GetNumStoredMessagesAllowedByRetrievalFilters(producer) != snapshot.Stored) return E_UNEXPECTED;
    return status;
}

bool IsDiagnosticAppendComplete(const DiagnosticQueueSnapshot& before, const DiagnosticQueueSnapshot& after) noexcept {
    return after.Accepted >= before.Accepted && after.Stored >= before.Stored &&
           after.Discarded == before.Discarded && after.Denied == before.Denied &&
           after.Accepted - before.Accepted == after.Stored - before.Stored;
}

bool IsDiagnosticClearComplete(const DiagnosticQueueSnapshot& copied, const DiagnosticQueueSnapshot& after) noexcept {
    return after.Accepted >= copied.Accepted && after.Discarded == copied.Discarded &&
           after.Denied == copied.Denied && after.Accepted - copied.Accepted == after.Stored;
}

template <class Message, class Snapshot, class Read>
static HRESULT _CopyDiagnosticBatch(Snapshot snapshot, Read read, DiagnosticQueueSnapshot& before,
                                    vector<DiagnosticMessage>& messages) noexcept {
    messages.clear();
    HRESULT status = snapshot(before);
    if (FAILED(status)) return status;
    if (before.Stored > 65536) return E_OUTOFMEMORY;
    vector<byte> scratch;
    size_t bytes = 0;
    for (uint64_t index = 0; index < before.Stored; ++index) {
        DiagnosticMessage message;
        status = _ReadDiagnosticMessage<Message>(read, index, scratch, message);
        if (FAILED(status)) break;
        bytes += message.Description.size();
        if (bytes > 64 * 1024 * 1024) {
            status = E_OUTOFMEMORY;
            break;
        }
        messages.push_back(std::move(message));
    }
    DiagnosticQueueSnapshot after{};
    if (SUCCEEDED(status)) status = snapshot(after);
    if (SUCCEEDED(status) && before != after) status = E_PENDING;
    if (FAILED(status)) messages.clear();
    return status;
}

HRESULT CopyDiagnosticBatch(ID3D12InfoQueue& queue, DiagnosticQueueSnapshot& snapshot,
                            vector<DiagnosticMessage>& messages) noexcept {
    return _CopyDiagnosticBatch<D3D12_MESSAGE>(
        [&](auto& result) { return ReadDiagnosticSnapshot(queue, result); },
        [&](UINT64 index, D3D12_MESSAGE* message, SIZE_T* size) { return queue.GetMessage(index, message, size); },
        snapshot, messages);
}

HRESULT CopyDiagnosticBatch(IDXGIInfoQueue& queue, GUID producer, DiagnosticQueueSnapshot& snapshot,
                            vector<DiagnosticMessage>& messages) noexcept {
    const HRESULT status = _CopyDiagnosticBatch<DXGI_INFO_QUEUE_MESSAGE>(
        [&](auto& result) { return ReadDiagnosticSnapshot(queue, producer, result); },
        [&](UINT64 index, DXGI_INFO_QUEUE_MESSAGE* message, SIZE_T* size) { return queue.GetMessage(producer, index, message, size); },
        snapshot, messages);
    for (auto& message : messages) message.Source = DiagnosticSource::Dxgi;
    return status;
}

static void _ReportCollectionFailure(DiagnosticState& channel, DiagnosticFailure failure,
                                     std::string_view reason, HRESULT result = E_UNEXPECTED) noexcept {
    if ((channel.ReportedFailures & (1u << static_cast<uint32_t>(failure))) == 0 ||
        channel.FailureResults[static_cast<size_t>(failure)] != result)
        _DispatchDiagnostic(channel, _CollectionFailure(channel, failure, reason, result));
}

static void _PollDiagnosticChannel(DiagnosticState& channel) noexcept {
    // The callback path must not inspect, clear, or maintain the storage queue.
    if (channel.Closed || channel.Mode == DiagnosticMode::Callback) return;
    const bool intact = _DiagnosticFiltersIntact(channel) &&
                        (channel.D3D12 ? channel.D3D12->GetMessageCountLimit() : channel.Dxgi->GetMessageCountLimit(channel.Producer)) == channel.ExpectedLimit;
    if (!intact) {
        _ReportCollectionFailure(channel, DiagnosticFailure::InconsistentState, "Diagnostic filters or capacity changed externally");
        return;
    }
    vector<DiagnosticMessage> messages;
    DiagnosticQueueSnapshot before{}, after{};
    HRESULT status = channel.D3D12 ? CopyDiagnosticBatch(*channel.D3D12.Get(), before, messages)
                                   : CopyDiagnosticBatch(*channel.Dxgi.Get(), channel.Producer, before, messages);
    if (FAILED(status)) {
        _ReportCollectionFailure(channel, status == E_PENDING ? DiagnosticFailure::InconsistentState : DiagnosticFailure::ReadFailed,
                                 "Diagnostic batch could not be copied completely; storage was not cleared", status);
        return;
    }
    if (before.Discarded > channel.Snapshot.Discarded)
        _ReportCollectionFailure(channel, DiagnosticFailure::Overflow, "Native diagnostic storage overflowed");
    else if (!IsDiagnosticAppendComplete(channel.Snapshot, before))
        _ReportCollectionFailure(channel, DiagnosticFailure::InconsistentState, "Diagnostic counters changed inconsistently between checkpoints");

    if (before.Stored != 0) {
        if (channel.D3D12)
            channel.D3D12->ClearStoredMessages();
        else
            channel.Dxgi->ClearStoredMessages(channel.Producer);
        status = channel.D3D12 ? ReadDiagnosticSnapshot(*channel.D3D12.Get(), after)
                               : ReadDiagnosticSnapshot(*channel.Dxgi.Get(), channel.Producer, after);
    } else {
        after = before;
    }
    if (FAILED(status)) {
        _ReportCollectionFailure(channel, DiagnosticFailure::ReadFailed, "Diagnostic counters could not be verified after clear", status);
    } else {
        if (!IsDiagnosticClearComplete(before, after))
            _ReportCollectionFailure(channel, after.Discarded > before.Discarded ? DiagnosticFailure::Overflow : DiagnosticFailure::InconsistentState,
                                     "Messages arriving during diagnostic clear do not match the retained messages");
        // After a fault this is only a best-effort restart; Healthy is never reset.
        channel.Snapshot = after;
    }
    for (auto& message : messages) {
        message.Channel = channel.Id;
        _DispatchDiagnostic(channel, message);
    }
}

bool FlushDiagnostics(const shared_ptr<DXGIDiagnosticContext>& context) noexcept {
    if (!context) return true;
    auto& registry = _Context(context);
    std::lock_guard change{registry.CollectionMutex};
    vector<shared_ptr<DiagnosticState>> channels;
    {
        std::lock_guard lock{registry.Mutex};
        channels = registry.Channels;
    }
    bool healthy = true;
    for (const auto& channel : channels) {
        _PollDiagnosticChannel(*channel);
        healthy = channel->Healthy.load() && healthy;
    }
    return healthy;
}

static bool _StartDiagnosticChannel(DiagnosticState& channel) noexcept {
    if (channel.Mode == DiagnosticMode::Callback) {
        DWORD cookie = 0;
        const HRESULT status = channel.CallbackQueue->RegisterMessageCallback(
            _DiagnosticCallback, D3D12_MESSAGE_CALLBACK_IGNORE_FILTERS, &channel, &cookie);
        if (SUCCEEDED(status))
            channel.Cookie = cookie;
        else
            _DispatchDiagnostic(channel, _CollectionFailure(channel, DiagnosticFailure::InitializationFailed,
                                                            "RegisterMessageCallback failed", status));
        return SUCCEEDED(status);
    }
    HRESULT status = S_OK;
    if (channel.D3D12) {
        channel.OldLimit = channel.D3D12->GetMessageCountLimit();
        status = channel.D3D12->SetMessageCountLimit(65536);
    } else {
        if (channel.Dxgi->GetMessageCountLimit(DXGI_DEBUG_APP) == 0)
            status = channel.Dxgi->AddApplicationMessage(DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO, "RadRay DXGI diagnostics initialized");
        if (SUCCEEDED(status)) status = channel.Dxgi->SetMessageCountLimit(channel.Producer, 65536);
        channel.ExpectedLimit = channel.Dxgi->GetMessageCountLimit(channel.Producer);
    }
    channel.LimitSet = SUCCEEDED(status);
    if (SUCCEEDED(status)) {
        status = channel.D3D12 ? channel.D3D12->PushEmptyStorageFilter() : channel.Dxgi->PushEmptyStorageFilter(channel.Producer);
        channel.StoragePushed = SUCCEEDED(status);
    }
    if (SUCCEEDED(status)) {
        status = channel.D3D12 ? channel.D3D12->PushEmptyRetrievalFilter() : channel.Dxgi->PushEmptyRetrievalFilter(channel.Producer);
        channel.RetrievalPushed = SUCCEEDED(status);
    }
    if (SUCCEEDED(status)) {
        channel.StorageDepth = channel.D3D12 ? channel.D3D12->GetStorageFilterStackSize() : channel.Dxgi->GetStorageFilterStackSize(channel.Producer);
        channel.RetrievalDepth = channel.D3D12 ? channel.D3D12->GetRetrievalFilterStackSize() : channel.Dxgi->GetRetrievalFilterStackSize(channel.Producer);
        status = channel.D3D12 ? ReadDiagnosticSnapshot(*channel.D3D12.Get(), channel.Snapshot)
                               : ReadDiagnosticSnapshot(*channel.Dxgi.Get(), channel.Producer, channel.Snapshot);
    }
    if (FAILED(status)) _DispatchDiagnostic(channel, _CollectionFailure(channel, DiagnosticFailure::InitializationFailed,
                                                                        "Diagnostic channel initialization failed", status));
    return SUCCEEDED(status);
}

static void _CloseDiagnosticChannel(DiagnosticState& channel) noexcept {
    channel.Closed = true;
    if (channel.Cookie) {
        const HRESULT status = channel.CallbackQueue->UnregisterMessageCallback(*channel.Cookie);
        if (FAILED(status)) {
            _DispatchDiagnostic(channel, _CollectionFailure(channel, DiagnosticFailure::ShutdownFailed, "UnregisterMessageCallback failed; callback context cannot be released", status));
            RADRAY_ABORT("UnregisterMessageCallback failed: {:#x}", static_cast<uint32_t>(status));
        }
        channel.Cookie.reset();
    }
    if (channel.RetrievalPushed) {
        if (channel.D3D12)
            channel.D3D12->PopRetrievalFilter();
        else
            channel.Dxgi->PopRetrievalFilter(channel.Producer);
    }
    if (channel.StoragePushed) {
        if (channel.D3D12)
            channel.D3D12->PopStorageFilter();
        else
            channel.Dxgi->PopStorageFilter(channel.Producer);
    }
    if (channel.LimitSet && channel.D3D12) {
        const HRESULT status = channel.D3D12->SetMessageCountLimit(channel.OldLimit);
        if (FAILED(status)) _DispatchDiagnostic(channel, _CollectionFailure(channel, DiagnosticFailure::ShutdownFailed, "Restoring diagnostic queue capacity failed", status));
    }
    channel.CallbackQueue.Reset();
    channel.D3D12.Reset();
    channel.Identity.Reset();
    channel.Dxgi.Reset();
}

static void _ReleaseDiagnosticChannel(const shared_ptr<DiagnosticState>& channel, const shared_ptr<DiagnosticReceiver>& receiver) noexcept {
    std::lock_guard change{channel->Context.CollectionMutex};
    _PollDiagnosticChannel(*channel);
    bool close = false;
    {
        auto& registry = channel->Context;
        std::lock_guard lock{registry.Mutex};
        close = --channel->Users == 0;
        if (close) std::erase(registry.Channels, channel);
    }
    if (close) _CloseDiagnosticChannel(*channel);
    {
        auto& registry = channel->Context;
        std::lock_guard lock{registry.Mutex};
        for (auto& [existing, users] : channel->Receivers) {
            if (existing == receiver) --users;
        }
        std::erase_if(channel->Receivers, [](const auto& entry) { return entry.second == 0; });
    }
}

static shared_ptr<DiagnosticState> _AcquireDiagnosticChannel(
    DiagnosticContext& context, const shared_ptr<DiagnosticReceiver>& receiver, ComPtr<ID3D12InfoQueue> queue,
    Nullable<ID3D12InfoQueue1*> callbackQueue, GUID producer = {}) {
    std::lock_guard change{context.CollectionMutex};
    auto channel = make_shared<DiagnosticState>(context);
    channel->D3D12 = queue;
    if (queue) {
        const HRESULT status = queue.As(&channel->Identity);
        if (FAILED(status)) {
            RADRAY_ERR_LOG("D3D12 diagnostic COM identity query failed: {:#x}", static_cast<uint32_t>(status));
            return nullptr;
        }
        channel->CallbackQueue = callbackQueue.Get();
        channel->Mode = callbackQueue ? DiagnosticMode::Callback : DiagnosticMode::Polling;
    } else {
        channel->Source = DiagnosticSource::Dxgi;
        channel->Producer = producer;
        const HRESULT status = DXGIGetDebugInterface1(0, IID_PPV_ARGS(channel->Dxgi.GetAddressOf()));
        if (FAILED(status)) {
            RADRAY_ERR_LOG("DXGIGetDebugInterface1(IDXGIInfoQueue) failed: {:#x}", static_cast<uint32_t>(status));
            return nullptr;
        }
    }
    {
        auto& registry = context;
        std::lock_guard lock{registry.Mutex};
        for (const auto& existing : registry.Channels) {
            if (existing->Source != channel->Source || existing->Identity != channel->Identity || existing->Producer != channel->Producer) continue;
            if (existing->Mode != channel->Mode) {
                RADRAY_ERR_LOG("Cannot change a shared diagnostic channel's active collection mode");
                return nullptr;
            }
            ++existing->Users;
            auto entry = std::ranges::find_if(existing->Receivers, [&](const auto& item) { return item.first == receiver; });
            if (entry == existing->Receivers.end())
                existing->Receivers.emplace_back(receiver, 1);
            else
                ++entry->second;
            return existing;
        }
        channel->Id = registry.NextId++;
        channel->Users = 1;
        channel->Receivers.emplace_back(receiver, 1);
    }
    if (!_StartDiagnosticChannel(*channel)) {
        _CloseDiagnosticChannel(*channel);
        return nullptr;
    }
    {
        auto& registry = context;
        std::lock_guard lock{registry.Mutex};
        registry.Channels.push_back(channel);
    }
    return channel;
}

class DiagnosticSubscription {
public:
    DiagnosticSubscription(shared_ptr<DXGIDiagnosticContext> context, shared_ptr<DiagnosticReceiver> receiver)
        : Context(std::move(context)), Receiver(std::move(receiver)) {}
    ~DiagnosticSubscription() noexcept {
        std::lock_guard lock{Receiver->Mutex};
        Receiver->Enabled = false;
    }
    shared_ptr<DXGIDiagnosticContext> Context;
    shared_ptr<DiagnosticReceiver> Receiver;
};

class DiagnosticSession {
public:
    explicit DiagnosticSession(shared_ptr<DXGIDiagnosticContext> context) : Context(std::move(context)) {}
    ~DiagnosticSession() noexcept {
        for (const auto& channel : Dxgi) {
            if (channel) _ReleaseDiagnosticChannel(channel, Receiver);
        }
        bool last = false;
        {
            auto& registry = _Context(Context);
            std::lock_guard lock{registry.Mutex};
            last = --Receiver->Sessions == 0;
            if (last) std::erase_if(registry.Loggers, [&](const auto& item) { return item.lock() == Receiver; });
        }
        if (last) {
            std::lock_guard lock{Receiver->Mutex};
            Receiver->Enabled = false;
        }
    }
    shared_ptr<DXGIDiagnosticContext> Context;
    shared_ptr<DiagnosticReceiver> Receiver;
    array<shared_ptr<DiagnosticState>, 2> Dxgi;
};

class DiagnosticChannel {
public:
    ~DiagnosticChannel() noexcept { _ReleaseDiagnosticChannel(State, Session->Receiver); }
    shared_ptr<DiagnosticSession> Session;
    shared_ptr<DiagnosticState> State;
};

shared_ptr<DXGIDiagnosticContext> CreateDiagnosticContext() {
    return make_shared<DiagnosticContext>();
}

bool FlushDiagnostics(const shared_ptr<DiagnosticSession>& session) noexcept {
    return !session || FlushDiagnostics(session->Context);
}

shared_ptr<DiagnosticSubscription> ObserveDiagnostics(
    const shared_ptr<DXGIDiagnosticContext>& context, shared_ptr<DiagnosticObserver> observer) {
    auto receiver = make_shared<DiagnosticReceiver>();
    receiver->Observer = std::move(observer);
    auto& registry = _Context(context);
    std::lock_guard lock{registry.Mutex};
    std::erase_if(registry.Observers, [](const auto& item) { return item.expired(); });
    registry.Observers.push_back(receiver);
    return make_shared<DiagnosticSubscription>(context, std::move(receiver));
}

Nullable<shared_ptr<DiagnosticSession>> CreateDiagnosticSession(const DXGIFactoryDescriptor& desc) {
    if (!desc.DiagnosticContext) {
        RADRAY_ERR_LOG("D3D12 validation requires an explicit DXGIDiagnosticContext");
        return nullptr;
    }
    auto session = make_shared<DiagnosticSession>(desc.DiagnosticContext);
    {
        auto& registry = _Context(session->Context);
        std::lock_guard lock{registry.Mutex};
        std::erase_if(registry.Loggers, [](const auto& item) { return item.expired(); });
        for (const auto& item : registry.Loggers) {
            auto receiver = item.lock();
            if (receiver && receiver->Callback.Get() == desc.LogCallback && receiver->UserData.Get() == desc.LogUserData) {
                session->Receiver = std::move(receiver);
                break;
            }
        }
        if (!session->Receiver) {
            session->Receiver = make_shared<DiagnosticReceiver>();
            session->Receiver->Callback = desc.LogCallback;
            session->Receiver->UserData = desc.LogUserData;
            registry.Loggers.push_back(session->Receiver);
        }
        ++session->Receiver->Sessions;
    }
    const GUID producers[]{DXGI_DEBUG_DXGI, DXGI_DEBUG_APP};
    for (size_t index = 0; index < std::size(producers); ++index) {
        session->Dxgi[index] = _AcquireDiagnosticChannel(_Context(session->Context), session->Receiver, nullptr, nullptr, producers[index]);
        if (!session->Dxgi[index]) return nullptr;
    }
    return session;
}

Nullable<shared_ptr<DiagnosticChannel>> AttachDiagnosticChannel(
    const shared_ptr<DiagnosticSession>& session, ComPtr<ID3D12InfoQueue> queue, Nullable<ID3D12InfoQueue1*> callbackQueue) {
    auto state = _AcquireDiagnosticChannel(_Context(session->Context), session->Receiver, queue, callbackQueue);
    if (!state) return nullptr;
    auto channel = make_shared<DiagnosticChannel>();
    channel->Session = session;
    channel->State = std::move(state);
    return channel;
}

static void _RefreshDxgiProducerCapacity(const shared_ptr<DiagnosticSession>& session) noexcept {
    if (!session) return;
    for (const auto& state : session->Dxgi) {
        auto& channel = *state;
        std::optional<DiagnosticMessage> failure;
        {
            const HRESULT status = channel.Dxgi->SetMessageCountLimit(channel.Producer, 65536);
            if (FAILED(status))
                failure = _CollectionFailure(channel, DiagnosticFailure::InconsistentState, "Configuring newly registered DXGI producer capacity failed", status);
            else
                channel.ExpectedLimit = channel.Dxgi->GetMessageCountLimit(channel.Producer);
        }
        if (failure) _DispatchDiagnostic(channel, *failure);
    }
}

DiagnosticTopologyScope::DiagnosticTopologyScope(shared_ptr<DiagnosticSession> session) noexcept : _session(std::move(session)) {
    if (!_session) return;
    _Context(_session->Context).CollectionMutex.lock();
    FlushDiagnostics(_session);
}

DiagnosticTopologyScope::~DiagnosticTopologyScope() noexcept {
    if (!_session) return;
    _RefreshDxgiProducerCapacity(_session);
    FlushDiagnostics(_session);
    _Context(_session->Context).CollectionMutex.unlock();
}

DiagnosticMode GetDiagnosticMode(const shared_ptr<DiagnosticChannel>& channel) noexcept { return channel->State->Mode; }
bool IsDiagnosticHealthy(const shared_ptr<DiagnosticChannel>& channel) noexcept { return channel->State->Healthy; }
bool IsDiagnosticHealthy(const shared_ptr<DiagnosticSession>& session) noexcept {
    return std::ranges::all_of(session->Dxgi, [](const auto& channel) { return channel->Healthy.load(); });
}

}  // namespace radray::render::d3d12::detail
