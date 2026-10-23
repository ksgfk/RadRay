#pragma once

#include <radray/render/backend/d3d12_helper.h>
#include <radray/render/rhi.h>
#include <directx/d3d12sdklayers.h>
#include <dxgidebug.h>

namespace radray::render::d3d12::detail {

enum class DiagnosticSource : uint8_t { D3D12,
                                        Dxgi };
enum class DiagnosticMode : uint8_t { Callback,
                                      Polling };
enum class DiagnosticFailure : uint8_t { None,
                                         ReadFailed,
                                         Overflow,
                                         InconsistentState,
                                         InitializationFailed,
                                         ShutdownFailed };

struct DiagnosticMessage {
    DiagnosticSource Source{DiagnosticSource::D3D12};
    uint64_t Channel{0};
    GUID Producer{};
    uint32_t Severity{0};
    uint32_t Category{0};
    int32_t Id{0};
    string Description;
    HRESULT Result{S_OK};
    DiagnosticFailure Failure{DiagnosticFailure::None};

    bool IsCollectionFailure() const noexcept { return Failure != DiagnosticFailure::None; }
};

class DiagnosticObserver {
public:
    virtual ~DiagnosticObserver() noexcept = default;
    virtual void OnDiagnostic(const DiagnosticMessage& message) noexcept = 0;
};

class DiagnosticSubscription;
class DiagnosticSession;
class DiagnosticChannel;

shared_ptr<DXGIDiagnosticContext> CreateDiagnosticContext();
shared_ptr<DiagnosticSubscription> ObserveDiagnostics(
    const shared_ptr<DXGIDiagnosticContext>& context, shared_ptr<DiagnosticObserver> observer);
Nullable<shared_ptr<DiagnosticSession>> CreateDiagnosticSession(const DXGIFactoryDescriptor& desc);
Nullable<shared_ptr<DiagnosticChannel>> AttachDiagnosticChannel(
    const shared_ptr<DiagnosticSession>& session, ComPtr<ID3D12InfoQueue> queue,
    Nullable<ID3D12InfoQueue1*> callbackQueue);
DiagnosticMode GetDiagnosticMode(const shared_ptr<DiagnosticChannel>& channel) noexcept;
bool IsDiagnosticHealthy(const shared_ptr<DiagnosticSession>& session) noexcept;
bool IsDiagnosticHealthy(const shared_ptr<DiagnosticChannel>& channel) noexcept;

struct DiagnosticQueueSnapshot {
    uint64_t Accepted{}, Stored{}, Discarded{}, Denied{};
    bool operator==(const DiagnosticQueueSnapshot&) const noexcept = default;
};

HRESULT ReadDiagnosticSnapshot(ID3D12InfoQueue& queue, DiagnosticQueueSnapshot& snapshot) noexcept;
HRESULT ReadDiagnosticSnapshot(IDXGIInfoQueue& queue, GUID producer, DiagnosticQueueSnapshot& snapshot) noexcept;
HRESULT CopyDiagnosticBatch(ID3D12InfoQueue& queue, DiagnosticQueueSnapshot& snapshot,
                            vector<DiagnosticMessage>& messages) noexcept;
HRESULT CopyDiagnosticBatch(IDXGIInfoQueue& queue, GUID producer, DiagnosticQueueSnapshot& snapshot,
                            vector<DiagnosticMessage>& messages) noexcept;
bool IsDiagnosticAppendComplete(const DiagnosticQueueSnapshot& before, const DiagnosticQueueSnapshot& after) noexcept;
bool IsDiagnosticClearComplete(const DiagnosticQueueSnapshot& copied, const DiagnosticQueueSnapshot& after) noexcept;

// Completes synchronous readers and their delivery only; never waits for GPU work
// or reads a callback channel's storage queue. False also reports earlier faults.
bool FlushDiagnostics(const shared_ptr<DXGIDiagnosticContext>& context) noexcept;
bool FlushDiagnostics(const shared_ptr<DiagnosticSession>& session) noexcept;

class DiagnosticTopologyScope {
public:
    explicit DiagnosticTopologyScope(shared_ptr<DiagnosticSession> session) noexcept;
    ~DiagnosticTopologyScope() noexcept;
    DiagnosticTopologyScope(const DiagnosticTopologyScope&) = delete;
    DiagnosticTopologyScope& operator=(const DiagnosticTopologyScope&) = delete;

private:
    shared_ptr<DiagnosticSession> _session;
};

}  // namespace radray::render::d3d12::detail
