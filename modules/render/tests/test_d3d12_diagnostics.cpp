#include "gpu_test_fixture.h"
#include <radray/scope_guard.h>
#ifdef RADRAY_ENABLE_D3D12
#include <dxgimessages.h>
#include <gtest/gtest-spi.h>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <thread>

namespace radray::render {
namespace {

// The forwarding COM spy belongs only to tests. It counts every base-queue
// call and injects races at the native read/clear boundary, without backend hooks.
class DiagnosticQueueSpy final : public ID3D12InfoQueue1 {
public:
    explicit DiagnosticQueueSpy(d3d12::ComPtr<ID3D12InfoQueue> native) : Native(std::move(native)) { Native.As(&Callback); }
    d3d12::ComPtr<ID3D12InfoQueue> Native;
    d3d12::ComPtr<ID3D12InfoQueue1> Callback;
    ULONG References{1};
    size_t StorageCalls{}, Clears{};
    HRESULT ReadResult{S_OK}, RegisterResult{S_OK};
    bool InsertDuringCopy{}, InsertBeforeClear{}, InsertAfterClear{};
    std::optional<UINT64> ReportedLimit;
    std::function<void()> BeforeUnregister;
    std::mutex CallbackMutex;
    std::condition_variable CallbackChanged;
    D3D12MessageFunc EmulatedCallback{};
    void* EmulatedContext{};
    size_t ActiveCallbacks{};
    HRESULT Emit(const char* message) {
        if (Callback) return Native->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, message);
        D3D12MessageFunc callback;
        void* context;
        {
            std::lock_guard lock{CallbackMutex};
            callback = EmulatedCallback;
            context = EmulatedContext;
            if (!callback) return E_FAIL;
            ++ActiveCallbacks;
        }
        callback(D3D12_MESSAGE_CATEGORY_APPLICATION_DEFINED, D3D12_MESSAGE_SEVERITY_INFO,
                 D3D12_MESSAGE_ID_STRING_FROM_APPLICATION, message, context);
        {
            std::lock_guard lock{CallbackMutex};
            --ActiveCallbacks;
        }
        CallbackChanged.notify_all();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ID3D12InfoQueue) || iid == __uuidof(ID3D12InfoQueue1)) {
            *result = static_cast<ID3D12InfoQueue1*>(this);
            AddRef();
            return S_OK;
        }
        *result = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++References; }
    ULONG STDMETHODCALLTYPE Release() override { return --References; }  // Test owns stack lifetime.
    HRESULT STDMETHODCALLTYPE GetMessage(UINT64 index, D3D12_MESSAGE* message, SIZE_T* size) override {
        ++StorageCalls;
        if (FAILED(ReadResult)) return ReadResult;
        const auto status = Native->GetMessage(index, message, size);
        if (message && std::exchange(InsertDuringCopy, false))
            Native->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "copy-race");
        return status;
    }
    void STDMETHODCALLTYPE ClearStoredMessages() override {
        ++StorageCalls;
        ++Clears;
        if (std::exchange(InsertBeforeClear, false)) Native->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "clear-race");
        Native->ClearStoredMessages();
        if (std::exchange(InsertAfterClear, false)) Native->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "retained");
    }
    HRESULT STDMETHODCALLTYPE RegisterMessageCallback(D3D12MessageFunc callback, D3D12_MESSAGE_CALLBACK_FLAGS flags,
                                                      void* context, DWORD* cookie) override {
        if (FAILED(RegisterResult)) return RegisterResult;
        if (Callback) return Callback->RegisterMessageCallback(callback, flags, context, cookie);
        std::lock_guard lock{CallbackMutex};
        EmulatedCallback = callback;
        EmulatedContext = context;
        *cookie = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnregisterMessageCallback(DWORD cookie) override {
        if (BeforeUnregister) BeforeUnregister();
        if (Callback) return Callback->UnregisterMessageCallback(cookie);
        std::unique_lock lock{CallbackMutex};
        EmulatedCallback = nullptr;
        CallbackChanged.wait(lock, [&] { return ActiveCallbacks == 0; });
        EmulatedContext = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMessageCountLimit(UINT64 MessageCountLimit) override {
        ++StorageCalls;
        return Native->SetMessageCountLimit(MessageCountLimit);
    }
    UINT64 STDMETHODCALLTYPE GetNumMessagesAllowedByStorageFilter() override {
        ++StorageCalls;
        return Native->GetNumMessagesAllowedByStorageFilter();
    }
    UINT64 STDMETHODCALLTYPE GetNumMessagesDeniedByStorageFilter() override {
        ++StorageCalls;
        return Native->GetNumMessagesDeniedByStorageFilter();
    }
    UINT64 STDMETHODCALLTYPE GetNumStoredMessages() override {
        ++StorageCalls;
        return Native->GetNumStoredMessages();
    }
    UINT64 STDMETHODCALLTYPE GetNumStoredMessagesAllowedByRetrievalFilter() override {
        ++StorageCalls;
        return Native->GetNumStoredMessagesAllowedByRetrievalFilter();
    }
    UINT64 STDMETHODCALLTYPE GetNumMessagesDiscardedByMessageCountLimit() override {
        ++StorageCalls;
        return Native->GetNumMessagesDiscardedByMessageCountLimit();
    }
    UINT64 STDMETHODCALLTYPE GetMessageCountLimit() override {
        ++StorageCalls;
        return ReportedLimit.value_or(Native->GetMessageCountLimit());
    }
    HRESULT STDMETHODCALLTYPE AddStorageFilterEntries(D3D12_INFO_QUEUE_FILTER* pFilter) override {
        ++StorageCalls;
        return Native->AddStorageFilterEntries(pFilter);
    }
    HRESULT STDMETHODCALLTYPE GetStorageFilter(D3D12_INFO_QUEUE_FILTER* pFilter, SIZE_T* pFilterByteLength) override {
        ++StorageCalls;
        return Native->GetStorageFilter(pFilter, pFilterByteLength);
    }
    void STDMETHODCALLTYPE ClearStorageFilter() override {
        ++StorageCalls;
        return Native->ClearStorageFilter();
    }
    HRESULT STDMETHODCALLTYPE PushEmptyStorageFilter() override {
        ++StorageCalls;
        return Native->PushEmptyStorageFilter();
    }
    HRESULT STDMETHODCALLTYPE PushCopyOfStorageFilter() override {
        ++StorageCalls;
        return Native->PushCopyOfStorageFilter();
    }
    HRESULT STDMETHODCALLTYPE PushStorageFilter(D3D12_INFO_QUEUE_FILTER* pFilter) override {
        ++StorageCalls;
        return Native->PushStorageFilter(pFilter);
    }
    void STDMETHODCALLTYPE PopStorageFilter() override {
        ++StorageCalls;
        return Native->PopStorageFilter();
    }
    UINT STDMETHODCALLTYPE GetStorageFilterStackSize() override {
        ++StorageCalls;
        return Native->GetStorageFilterStackSize();
    }
    HRESULT STDMETHODCALLTYPE AddRetrievalFilterEntries(D3D12_INFO_QUEUE_FILTER* pFilter) override {
        ++StorageCalls;
        return Native->AddRetrievalFilterEntries(pFilter);
    }
    HRESULT STDMETHODCALLTYPE GetRetrievalFilter(D3D12_INFO_QUEUE_FILTER* pFilter, SIZE_T* pFilterByteLength) override {
        ++StorageCalls;
        return Native->GetRetrievalFilter(pFilter, pFilterByteLength);
    }
    void STDMETHODCALLTYPE ClearRetrievalFilter() override {
        ++StorageCalls;
        return Native->ClearRetrievalFilter();
    }
    HRESULT STDMETHODCALLTYPE PushEmptyRetrievalFilter() override {
        ++StorageCalls;
        return Native->PushEmptyRetrievalFilter();
    }
    HRESULT STDMETHODCALLTYPE PushCopyOfRetrievalFilter() override {
        ++StorageCalls;
        return Native->PushCopyOfRetrievalFilter();
    }
    HRESULT STDMETHODCALLTYPE PushRetrievalFilter(D3D12_INFO_QUEUE_FILTER* pFilter) override {
        ++StorageCalls;
        return Native->PushRetrievalFilter(pFilter);
    }
    void STDMETHODCALLTYPE PopRetrievalFilter() override {
        ++StorageCalls;
        return Native->PopRetrievalFilter();
    }
    UINT STDMETHODCALLTYPE GetRetrievalFilterStackSize() override {
        ++StorageCalls;
        return Native->GetRetrievalFilterStackSize();
    }
    HRESULT STDMETHODCALLTYPE AddMessage(D3D12_MESSAGE_CATEGORY Category, D3D12_MESSAGE_SEVERITY Severity, D3D12_MESSAGE_ID ID, LPCSTR pDescription) override {
        ++StorageCalls;
        return Native->AddMessage(Category, Severity, ID, pDescription);
    }
    HRESULT STDMETHODCALLTYPE AddApplicationMessage(D3D12_MESSAGE_SEVERITY Severity, LPCSTR pDescription) override {
        ++StorageCalls;
        return Native->AddApplicationMessage(Severity, pDescription);
    }
    HRESULT STDMETHODCALLTYPE SetBreakOnCategory(D3D12_MESSAGE_CATEGORY Category, BOOL bEnable) override {
        ++StorageCalls;
        return Native->SetBreakOnCategory(Category, bEnable);
    }
    HRESULT STDMETHODCALLTYPE SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY Severity, BOOL bEnable) override {
        ++StorageCalls;
        return Native->SetBreakOnSeverity(Severity, bEnable);
    }
    HRESULT STDMETHODCALLTYPE SetBreakOnID(D3D12_MESSAGE_ID ID, BOOL bEnable) override {
        ++StorageCalls;
        return Native->SetBreakOnID(ID, bEnable);
    }
    BOOL STDMETHODCALLTYPE GetBreakOnCategory(D3D12_MESSAGE_CATEGORY Category) override {
        ++StorageCalls;
        return Native->GetBreakOnCategory(Category);
    }
    BOOL STDMETHODCALLTYPE GetBreakOnSeverity(D3D12_MESSAGE_SEVERITY Severity) override {
        ++StorageCalls;
        return Native->GetBreakOnSeverity(Severity);
    }
    BOOL STDMETHODCALLTYPE GetBreakOnID(D3D12_MESSAGE_ID ID) override {
        ++StorageCalls;
        return Native->GetBreakOnID(ID);
    }
    void STDMETHODCALLTYPE SetMuteDebugOutput(BOOL bMute) override {
        ++StorageCalls;
        return Native->SetMuteDebugOutput(bMute);
    }
    BOOL STDMETHODCALLTYPE GetMuteDebugOutput() override {
        ++StorageCalls;
        return Native->GetMuteDebugOutput();
    }
};

class D3D12DiagnosticsTest : public testing::TestWithParam<bool> {
protected:
    struct CleanupMessage {
        d3d12::ComPtr<ID3D12InfoQueue> Queue;
        ~CleanupMessage() { Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "fixture cleanup error"); }
    };
    test::DeviceContext Context;
    unique_ptr<CleanupMessage> Cleanup;
    void SetUp() override {
        if (!test::TryCreateDevice(RenderBackend::D3D12, Context, true, false)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(RenderBackend::D3D12))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
        auto& device = *static_cast<d3d12::DeviceD3D12*>(Context.Device.get());
        if (GetParam()) {
            d3d12::ComPtr<ID3D12InfoQueue> queue;
            ASSERT_TRUE(SUCCEEDED(device._device.As(&queue)));
            device._diagnosticChannel.reset();
            auto channel = d3d12::detail::AttachDiagnosticChannel(device._diagnostics, queue, nullptr);
            ASSERT_TRUE(channel);
            device._diagnosticChannel = channel.Release();
            Context.DiagnosticMode = "polling";
            Context.DiagnosticSelection = "base-interface-only";
            Context.DiagnosticSelfTest = Context.Diagnostics->SelfTest(queue.Get());
            ASSERT_TRUE(Context.DiagnosticSelfTest);
        } else {
            EXPECT_EQ(Context.DiagnosticMode, Context.DiagnosticInterfaceResult == "0x00000000" ? "callback" : "polling");
        }
    }
    void TearDown() override {
        Context.WaitAllQueues();
        Cleanup.reset();
        Context.Reset();
    }
};

TEST_P(D3D12DiagnosticsTest, LastOperationErrorIsSettledByReset) {
    RecordProperty("evidence_class", "isolated_expected_error");
    auto& device = *static_cast<d3d12::DeviceD3D12*>(Context.Device.get());
    d3d12::ComPtr<ID3D12InfoQueue> queue;
    ASSERT_TRUE(SUCCEEDED(device._device.As(&queue)));
    Context.Diagnostics->Expect({.SourceType = test::DiagnosticProbe::Source::D3D12,
                                 .Id = D3D12_MESSAGE_ID_STRING_FROM_APPLICATION,
                                 .Severity = D3D12_MESSAGE_SEVERITY_ERROR,
                                 .Begin = Context.Diagnostics->CheckpointAndMark()});
    ASSERT_EQ(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "last operation error"), S_OK);
}

TEST_P(D3D12DiagnosticsTest, FixtureCleanupErrorIsSettledByReset) {
    RecordProperty("evidence_class", "isolated_expected_error");
    auto& device = *static_cast<d3d12::DeviceD3D12*>(Context.Device.get());
    Cleanup = make_unique<CleanupMessage>();
    ASSERT_TRUE(SUCCEEDED(device._device.As(&Cleanup->Queue)));
    Context.Diagnostics->Expect({.SourceType = test::DiagnosticProbe::Source::D3D12,
                                 .Id = D3D12_MESSAGE_ID_STRING_FROM_APPLICATION,
                                 .Severity = D3D12_MESSAGE_SEVERITY_ERROR,
                                 .Begin = Context.Diagnostics->CheckpointAndMark()});
}

TEST(D3D12DiagnosticFinalizer, RejectsUnexpectedErrorsFaultsAndWrongIntervals) {
    using namespace d3d12::detail;
    std::atomic<uint32_t> errors{0};
    test::DiagnosticProbe probe{errors, DXGIDiagnosticContext::Create().Unwrap()};
    probe.OnDiagnostic({.Severity = D3D12_MESSAGE_SEVERITY_ERROR, .Id = 7, .Description = "unexpected"});
    EXPECT_NONFATAL_FAILURE(probe.Verify(), "Unexpected or ambiguous GPU diagnostic");
    probe.Messages.clear();
    probe.OnDiagnostic({.Severity = D3D12_MESSAGE_SEVERITY_ERROR, .Description = "read failed", .Result = E_FAIL, .Failure = DiagnosticFailure::ReadFailed});
    EXPECT_NONFATAL_FAILURE(probe.Verify(), "Unexpected or ambiguous GPU diagnostic");
    probe.Messages.clear();
    probe.Expect({.SourceType = DiagnosticSource::D3D12, .Id = 7, .Severity = D3D12_MESSAGE_SEVERITY_ERROR, .Begin = 1, .Count = 0});
    probe.OnDiagnostic({.Severity = D3D12_MESSAGE_SEVERITY_ERROR, .Id = 7, .Description = "outside interval"});
    EXPECT_NONFATAL_FAILURE(probe.Verify(), "Unexpected or ambiguous GPU diagnostic");
}

TEST(D3D12DiagnosticFinalizer, UnexpectedLastNativeErrorFailsGTestAtReset) {
    EXPECT_EXIT(([] {
                    testing::internal::CaptureStdout();
                    test::DeviceContext context;
                    if (!test::TryCreateDevice(RenderBackend::D3D12, context, true, false)) _exit(2);
                    auto& device = *static_cast<d3d12::DeviceD3D12*>(context.Device.get());
                    d3d12::ComPtr<ID3D12InfoQueue> queue;
                    if (FAILED(device._device.As(&queue))) _exit(3);
                    queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "unclaimed last native error");
                    queue.Reset();
                    context.Reset();
                    testing::internal::GetCapturedStdout();
                    _exit(testing::Test::HasFailure() && context.ValidationErrors == 1 ? 13 : 0);
                }()),
                testing::ExitedWithCode(13), "");
}

TEST_P(D3D12DiagnosticsTest, StructuredMessagesAndAllocatorResetError) {
    testing::Test::RecordProperty("diagnostic_probe", "allocator_reset");
    testing::Test::RecordProperty("evidence_class", "isolated_expected_error");
    testing::Test::RecordProperty("expected_validation_errors", 1);
    auto& device = *static_cast<d3d12::DeviceD3D12*>(Context.Device.get());
    d3d12::ComPtr<ID3D12InfoQueue> queue;
    d3d12::ComPtr<ID3D12InfoQueue1> callbackQueue;
    ASSERT_TRUE(SUCCEEDED(device._device.As(&queue)));
    const bool callback = Context.DiagnosticMode == "callback";
    if (callback) ASSERT_TRUE(SUCCEEDED(queue.As(&callbackQueue)));
    auto sharedChannel = d3d12::detail::AttachDiagnosticChannel(device._diagnostics, queue, callbackQueue.Get()).Unwrap();
    device._diagnosticChannel.reset();
    ASSERT_TRUE(Context.Diagnostics->SelfTest(queue.Get()));
    device._diagnosticChannel = std::move(sharedChannel);
    const auto start = Context.Diagnostics->CheckpointAndMark();
    constexpr int32_t markerId = D3D12_MESSAGE_ID_STRING_FROM_APPLICATION;
    ASSERT_TRUE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "structured smoke marker")));
    EXPECT_EQ(Context.Diagnostics->Count(start, d3d12::detail::DiagnosticSource::D3D12, markerId), callback ? 1u : 0u);
    EXPECT_TRUE(Context.Diagnostics->Checkpoint());
    EXPECT_EQ(Context.Diagnostics->Count(start, d3d12::detail::DiagnosticSource::D3D12, markerId), 1u);
    EXPECT_TRUE(Context.Diagnostics->Checkpoint());
    EXPECT_EQ(Context.Diagnostics->Count(start, d3d12::detail::DiagnosticSource::D3D12, markerId), 1u);

    d3d12::ComPtr<ID3D12CommandAllocator> allocator;
    d3d12::ComPtr<ID3D12GraphicsCommandList> commands;
    ASSERT_TRUE(SUCCEEDED(device._device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
    ASSERT_TRUE(SUCCEEDED(device._device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commands))));
    const auto expected = Context.Diagnostics->Expect({.SourceType = test::DiagnosticProbe::Source::D3D12,
                                                       .Id = D3D12_MESSAGE_ID_COMMAND_ALLOCATOR_CANNOT_RESET,
                                                       .Severity = D3D12_MESSAGE_SEVERITY_ERROR,
                                                       .Begin = start});
    EXPECT_EQ(allocator->Reset(), E_FAIL);
    Context.Diagnostics->EndExpectation(expected);
    EXPECT_TRUE(Context.Diagnostics->Checkpoint());
    EXPECT_EQ(Context.Diagnostics->Count(start, d3d12::detail::DiagnosticSource::D3D12, D3D12_MESSAGE_ID_COMMAND_ALLOCATOR_CANNOT_RESET), 1u);
    EXPECT_EQ(Context.ValidationErrors.load(), 1u);
    EXPECT_TRUE(d3d12::detail::IsDiagnosticHealthy(device._diagnosticChannel));
    ASSERT_TRUE(SUCCEEDED(commands->Close()));
    if (callback) {
        ASSERT_TRUE(SUCCEEDED(queue->PushEmptyStorageFilter()));
        const auto oldLimit = queue->GetMessageCountLimit();
        auto restoreFilter = MakeScopeGuard([&] { queue->SetMessageCountLimit(oldLimit); queue->PopStorageFilter(); });
        ASSERT_TRUE(SUCCEEDED(queue->SetMessageCountLimit(1)));
        ASSERT_TRUE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "callback overflow one")));
        ASSERT_TRUE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "callback overflow two")));
        EXPECT_GT(queue->GetNumMessagesDiscardedByMessageCountLimit(), 0u);
        EXPECT_TRUE(Context.Diagnostics->HasMarker(d3d12::detail::DiagnosticSource::D3D12, "callback overflow two"));
        Context.Diagnostics->Checkpoint();
        EXPECT_TRUE(d3d12::detail::IsDiagnosticHealthy(device._diagnosticChannel));
    }
    {
        std::lock_guard lock{Context.Diagnostics->Mutex};
        const auto& messages = Context.Diagnostics->Messages;
        const auto found = std::find_if(messages.begin() + start, messages.end(), [](const auto& message) {
            return message.Source == d3d12::detail::DiagnosticSource::D3D12 && message.Id == D3D12_MESSAGE_ID_COMMAND_ALLOCATOR_CANNOT_RESET;
        });
        ASSERT_NE(found, messages.end());
        EXPECT_NE(found->Channel, 0u);
        EXPECT_EQ(found->Severity, D3D12_MESSAGE_SEVERITY_ERROR);
        EXPECT_EQ(found->Category, D3D12_MESSAGE_CATEGORY_EXECUTION);
        EXPECT_FALSE(found->Description.empty());
        EXPECT_FALSE(found->IsCollectionFailure());
    }
}

INSTANTIATE_TEST_SUITE_P(NativeModes, D3D12DiagnosticsTest, testing::Values(false, true),
                         [](const testing::TestParamInfo<bool>& info) { return info.param ? "BaseInterfaceOnly" : "NativeSelection"; });

TEST(D3D12DiagnosticsSmoke, DxgiApplicationAndNativeError) {
    testing::Test::RecordProperty("diagnostic_probe", "dxgi_native_error");
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::D3D12, context, true, false)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(RenderBackend::D3D12))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    testing::Test::RecordProperty("evidence_class", "isolated_expected_error");
    testing::Test::RecordProperty("expected_validation_errors", 1);
    auto shutdown = MakeScopeGuard([&] { context.Reset(); });
    d3d12::ComPtr<IDXGIInfoQueue> queue;
    ASSERT_TRUE(SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&queue))));
    const auto start = context.Diagnostics->CheckpointAndMark();
    ASSERT_TRUE(SUCCEEDED(queue->AddApplicationMessage(DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO, "DXGI application smoke marker")));
    auto* factory = static_cast<d3d12::DXGIFactoryImpl*>(context.Factory.get());
    const auto expected = context.Diagnostics->Expect({.SourceType = test::DiagnosticProbe::Source::Dxgi, .Producer = DXGI_DEBUG_DXGI, .Id = DXGI_MSG_IDXGIFactory_MakeWindowAssociation_InvalidFlags, .Severity = DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, .Begin = start});
    EXPECT_EQ(factory->_factory->MakeWindowAssociation(nullptr, 0x80000000u), DXGI_ERROR_INVALID_CALL);
    context.Diagnostics->EndExpectation(expected);
    EXPECT_EQ(context.Diagnostics->Count(start, d3d12::detail::DiagnosticSource::Dxgi, DXGI_MSG_IDXGIFactory_MakeWindowAssociation_InvalidFlags), 1u);
    EXPECT_TRUE(context.Diagnostics->Checkpoint());
    EXPECT_EQ(context.Diagnostics->Count(start, d3d12::detail::DiagnosticSource::Dxgi, DXGI_MSG_IDXGIFactory_MakeWindowAssociation_InvalidFlags), 1u);
    EXPECT_EQ(context.ValidationErrors.load(), 1u);
    std::lock_guard lock{context.Diagnostics->Mutex};
    bool application = false, nativeError = false;
    for (size_t index = start; index < context.Diagnostics->Messages.size(); ++index) {
        const auto& message = context.Diagnostics->Messages[index];
        if (message.Source != d3d12::detail::DiagnosticSource::Dxgi) continue;
        if (message.Description.find("DXGI application smoke marker") != string::npos) {
            application = true;
            EXPECT_EQ(message.Producer, DXGI_DEBUG_APP);
            EXPECT_EQ(message.Severity, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO);
        } else if (message.Id == DXGI_MSG_IDXGIFactory_MakeWindowAssociation_InvalidFlags) {
            nativeError = true;
            EXPECT_EQ(message.Producer, DXGI_DEBUG_DXGI);
            EXPECT_EQ(message.Severity, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR);
            EXPECT_NE(message.Channel, 0u);
        }
    }
    EXPECT_TRUE(application);
    EXPECT_TRUE(nativeError);
}

TEST(D3D12DiagnosticsSmoke, CapacityContractViolationIsIncomplete) {
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::D3D12, context, true, false)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(RenderBackend::D3D12))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    testing::Test::RecordProperty("evidence_class", "isolated_collection_failure");
    testing::Test::RecordProperty("expected_collection_failures", 1);
    auto shutdown = MakeScopeGuard([&] { context.Reset(); });
    auto& device = *static_cast<d3d12::DeviceD3D12*>(context.Device.get());
    d3d12::ComPtr<ID3D12InfoQueue> queue;
    ASSERT_TRUE(SUCCEEDED(device._device.As(&queue)));
    device._diagnosticChannel.reset();
    device._diagnosticChannel = d3d12::detail::AttachDiagnosticChannel(device._diagnostics, queue, nullptr).Unwrap();
    context.DiagnosticMode = "polling";
    context.DiagnosticSelection = "base-interface-only";
    context.DiagnosticSelfTest = context.Diagnostics->SelfTest(queue.Get());
    ASSERT_TRUE(context.DiagnosticSelfTest);
    context.Diagnostics->Expect({.SourceType = test::DiagnosticProbe::Source::D3D12,
                                 .Severity = D3D12_MESSAGE_SEVERITY_ERROR,
                                 .Fault = test::DiagnosticProbe::Failure::InconsistentState,
                                 .Result = E_UNEXPECTED,
                                 .Begin = context.Diagnostics->CheckpointAndMark()});
    ASSERT_TRUE(SUCCEEDED(queue->SetMessageCountLimit(1)));
    ASSERT_TRUE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "overflow one")));
    ASSERT_TRUE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "overflow two")));
    ASSERT_GT(queue->GetNumMessagesDiscardedByMessageCountLimit(), 0u);
    context.Diagnostics->Checkpoint();
    EXPECT_FALSE(d3d12::detail::IsDiagnosticHealthy(device._diagnosticChannel));
    EXPECT_EQ(context.Diagnostics->Failures.load(), 1u);
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}

class D3D12DiagnosticsReaders : public testing::Test {
protected:
    d3d12::ComPtr<IDXGIFactory4> Factory;
    d3d12::ComPtr<ID3D12Device> Device;
    d3d12::ComPtr<ID3D12InfoQueue> Queue;
    d3d12::ComPtr<IDXGIInfoQueue> Dxgi;
    void SetUp() override {
        d3d12::ComPtr<ID3D12Debug> debug;
        ASSERT_TRUE(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))));
        debug->EnableDebugLayer();
        ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&Factory))));
        uint32_t adapterIndex = 0;
        if (const char* value = std::getenv("RADRAY_TEST_D3D12_ADAPTER_INDEX")) {
            ASSERT_EQ(std::from_chars(value, value + std::strlen(value), adapterIndex).ec, std::errc{});
        }
        d3d12::ComPtr<IDXGIAdapter1> adapter;
        ASSERT_TRUE(SUCCEEDED(Factory->EnumAdapters1(adapterIndex, &adapter)));
        ASSERT_TRUE(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&Device))));
        ASSERT_TRUE(SUCCEEDED(Device.As(&Queue)));
        ASSERT_TRUE(SUCCEEDED(Queue->PushEmptyStorageFilter()));
        ASSERT_TRUE(SUCCEEDED(Queue->PushEmptyRetrievalFilter()));
        ASSERT_TRUE(SUCCEEDED(Queue->SetMessageCountLimit(4)));
        ASSERT_TRUE(SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&Dxgi))));
        ASSERT_TRUE(SUCCEEDED(Dxgi->AddApplicationMessage(DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO, "reader initialize")));
    }
    void TearDown() override {
        if (Queue) {
            Queue->PopRetrievalFilter();
            Queue->PopStorageFilter();
        }
        // These isolated reader tests own both producers; do not leak unread
        // deliberately generated diagnostics into the next case's observer.
        if (Dxgi) {
            Dxgi->ClearStoredMessages(DXGI_DEBUG_DXGI);
            Dxgi->ClearStoredMessages(DXGI_DEBUG_APP);
        }
    }
};

TEST_F(D3D12DiagnosticsReaders, CopyAndClearDoesNotRepeatMessages) {
    using namespace d3d12::detail;
    Queue->ClearStoredMessages();
    DiagnosticQueueSnapshot previous{}, copied{}, after{};
    ASSERT_EQ(ReadDiagnosticSnapshot(*Queue.Get(), previous), S_OK);
    vector<DiagnosticMessage> messages;
    for (unsigned sequence = 0; sequence < 20; ++sequence) {
        const auto marker = fmt::format("checked-clear-{}", sequence);
        ASSERT_TRUE(SUCCEEDED(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, marker.c_str())));
        ASSERT_EQ(CopyDiagnosticBatch(*Queue.Get(), copied, messages), S_OK);
        EXPECT_TRUE(IsDiagnosticAppendComplete(previous, copied));
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_EQ(messages.front().Description, marker);
        Queue->ClearStoredMessages();
        ASSERT_EQ(ReadDiagnosticSnapshot(*Queue.Get(), after), S_OK);
        EXPECT_TRUE(IsDiagnosticClearComplete(copied, after));
        previous = after;
        ASSERT_EQ(CopyDiagnosticBatch(*Queue.Get(), copied, messages), S_OK);
        EXPECT_TRUE(messages.empty());
    }
    for (unsigned sequence = 0; sequence < 7; ++sequence)
        ASSERT_TRUE(SUCCEEDED(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "overflow")));
    ASSERT_EQ(CopyDiagnosticBatch(*Queue.Get(), copied, messages), S_OK);
    EXPECT_GT(copied.Discarded, previous.Discarded);
    EXPECT_FALSE(IsDiagnosticAppendComplete(previous, copied));
    RecordProperty("diagnostic_algorithm", "checked_clear");
    RecordProperty("diagnostic_algorithm_result", "passed");
}

TEST_F(D3D12DiagnosticsReaders, FailedAndChangingCopiesNeverClear) {
    using namespace d3d12::detail;
    DiagnosticQueueSpy spy{Queue};
    Queue->ClearStoredMessages();
    ASSERT_EQ(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "original"), S_OK);
    DiagnosticQueueSnapshot snapshot{};
    vector<DiagnosticMessage> messages;
    spy.ReadResult = E_FAIL;
    EXPECT_EQ(CopyDiagnosticBatch(spy, snapshot, messages), E_FAIL);
    EXPECT_TRUE(messages.empty());
    EXPECT_EQ(spy.Clears, 0u);
    spy.ReadResult = S_OK;
    spy.InsertDuringCopy = true;
    EXPECT_EQ(CopyDiagnosticBatch(spy, snapshot, messages), E_PENDING);
    EXPECT_TRUE(messages.empty());
    EXPECT_EQ(spy.Clears, 0u);
    EXPECT_EQ(Queue->GetNumStoredMessages(), 2u);
    EXPECT_EQ(CopyDiagnosticBatch(spy, snapshot, messages), S_OK);
    EXPECT_EQ(messages.size(), 2u);
}

TEST_F(D3D12DiagnosticsReaders, CollectorDetectsRacesOverflowAndExternalClear) {
    using namespace d3d12::detail;
    // Each scenario starts a distinct capture session: incompleteness is sticky.
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        SCOPED_TRACE(scenario);
        DiagnosticQueueSpy spy{Queue};
        Queue->ClearStoredMessages();
        std::atomic<uint32_t> errors{0};
        auto diagnosticContext = DXGIDiagnosticContext::Create().Unwrap();
        auto probe = make_shared<test::DiagnosticProbe>(errors, diagnosticContext);
        auto subscription = ObserveDiagnostics(diagnosticContext, probe);
        auto session = CreateDiagnosticSession({.LogCallback = [](LogLevel, std::string_view, void*) {}, .DiagnosticContext = diagnosticContext}).Unwrap();
        auto channel = AttachDiagnosticChannel(session, &spy, nullptr).Unwrap();
        ASSERT_TRUE(FlushDiagnostics(diagnosticContext));
        const auto expectedFault = scenario == 0 ? DiagnosticFailure::ReadFailed : (scenario == 4 || scenario == 6) ? DiagnosticFailure::Overflow
                                                                                                                    : DiagnosticFailure::InconsistentState;
        const auto expectedResult = scenario == 0 ? E_FAIL : scenario == 1 ? E_PENDING
                                                                           : E_UNEXPECTED;
        if (scenario != 3) probe->Expect({.SourceType = DiagnosticSource::D3D12,
                                          .Severity = D3D12_MESSAGE_SEVERITY_ERROR,
                                          .Fault = expectedFault,
                                          .Result = expectedResult});
        ASSERT_EQ(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "original"), S_OK);
        if (scenario == 0) spy.ReadResult = E_FAIL;
        if (scenario == 1) spy.InsertDuringCopy = true;
        if (scenario == 2) spy.InsertBeforeClear = true;
        if (scenario == 3) spy.InsertAfterClear = true;
        if (scenario == 4 || scenario == 6) {
            // Use a small real native queue while exposing the configured limit
            // through the spy, so the collector exercises its overflow branch.
            ASSERT_EQ(Queue->SetMessageCountLimit(4), S_OK);
            spy.ReportedLimit = 65536;
            for (unsigned i = 0; i < 7; ++i) ASSERT_EQ(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "overflow"), S_OK);
        }
        if (scenario == 6) {
            probe->Expect({.SourceType = DiagnosticSource::D3D12, .Severity = D3D12_MESSAGE_SEVERITY_ERROR, .Fault = DiagnosticFailure::InconsistentState, .Result = E_UNEXPECTED});
            // Restore capacity so the clear-race message is appended, not evicted.
            ASSERT_EQ(Queue->SetMessageCountLimit(65536), S_OK);
            spy.InsertBeforeClear = true;
        }
        if (scenario == 5) Queue->ClearStoredMessages();
        const auto clears = spy.Clears;
        EXPECT_EQ(probe->Checkpoint(), scenario == 3);
        if (scenario < 2) EXPECT_EQ(spy.Clears, clears);
        if (scenario == 3) EXPECT_EQ(Queue->GetNumStoredMessages(), 1u);
        spy.ReadResult = S_OK;
        EXPECT_EQ(probe->Checkpoint(), scenario == 3);
        channel.reset();
        session.reset();
        subscription.reset();
        EXPECT_TRUE(probe->Verify());
        EXPECT_EQ(errors.load(), 0u);
    }
}

TEST(D3D12DiagnosticCounters, RejectsCounterRegressionAndUnexplainedChanges) {
    using namespace d3d12::detail;
    const DiagnosticQueueSnapshot before{10, 3, 0, 0};
    EXPECT_TRUE(IsDiagnosticAppendComplete(before, {12, 5, 0, 0}));
    for (const auto after : {DiagnosticQueueSnapshot{9, 3, 0, 0}, {10, 0, 0, 0}, {12, 4, 0, 0}, {12, 5, 1, 0}, {12, 5, 0, 1}})
        EXPECT_FALSE(IsDiagnosticAppendComplete(before, after));
    EXPECT_TRUE(IsDiagnosticClearComplete(before, {11, 1, 0, 0}));
    EXPECT_FALSE(IsDiagnosticClearComplete(before, {11, 0, 0, 0}));
    EXPECT_FALSE(IsDiagnosticClearComplete(before, {9, 0, 0, 0}));
}

TEST_F(D3D12DiagnosticsReaders, CheckpointsObserversAndFaultsStayWithinTheirContext) {
    using namespace d3d12::detail;
    auto active = DXGIDiagnosticContext::Create().Unwrap();
    auto idle = DXGIDiagnosticContext::Create().Unwrap();
    std::atomic<uint32_t> activeErrors{0}, idleErrors{0};
    auto activeProbe = make_shared<test::DiagnosticProbe>(activeErrors, active);
    auto idleProbe = make_shared<test::DiagnosticProbe>(idleErrors, idle);
    auto activeSubscription = ObserveDiagnostics(active, activeProbe);
    auto idleSubscription = ObserveDiagnostics(idle, idleProbe);
    auto session = CreateDiagnosticSession({.LogCallback = [](LogLevel, std::string_view, void*) {}, .DiagnosticContext = active}).Unwrap();
    DiagnosticQueueSpy spy{Queue};
    auto channel = AttachDiagnosticChannel(session, &spy, nullptr).Unwrap();
    ASSERT_TRUE(activeProbe->Checkpoint());
    ASSERT_EQ(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "explicit context marker"), S_OK);
    EXPECT_TRUE(idleProbe->Checkpoint());
    EXPECT_FALSE(activeProbe->HasMarker(DiagnosticSource::D3D12, "explicit context marker"));
    EXPECT_TRUE(activeProbe->Checkpoint());
    EXPECT_TRUE(activeProbe->HasMarker(DiagnosticSource::D3D12, "explicit context marker"));
    EXPECT_TRUE(idleProbe->Messages.empty());

    activeProbe->Expect({.SourceType = DiagnosticSource::D3D12, .Severity = D3D12_MESSAGE_SEVERITY_ERROR, .Fault = DiagnosticFailure::ReadFailed, .Result = E_FAIL});
    ASSERT_EQ(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "read failure"), S_OK);
    spy.ReadResult = E_FAIL;
    EXPECT_FALSE(activeProbe->Checkpoint());
    EXPECT_TRUE(idleProbe->Checkpoint());
    spy.ReadResult = S_OK;
    EXPECT_FALSE(activeProbe->Checkpoint());
    channel.reset();
    session.reset();
    activeSubscription.reset();
    idleSubscription.reset();
    EXPECT_TRUE(activeProbe->Verify());
    EXPECT_TRUE(idleProbe->Verify());
    EXPECT_EQ(activeProbe->Failures.load(), 1u);
    EXPECT_EQ(idleProbe->Failures.load(), 0u);
}

TEST(D3D12DiagnosticsSmoke, DebugFactoryRequiresExplicitContext) {
    EXPECT_FALSE(DXGIFactory::Create({.IsEnableDebugLayer = true, .DiagnosticContext = {}}));
}

TEST(D3D12DiagnosticsSmoke, DiagnosticContextSurvivesFactoryUntilFinalDeviceCleanup) {
    test::DeviceContext context;
    ASSERT_TRUE(test::TryCreateDevice(RenderBackend::D3D12, context, true, false));
    weak_ptr<DXGIDiagnosticContext> lifetime = context.DiagnosticContext;
    context.Factory.reset();
    EXPECT_FALSE(lifetime.expired());
    auto& device = *static_cast<d3d12::DeviceD3D12*>(context.Device.get());
    d3d12::ComPtr<ID3D12InfoQueue> queue;
    ASSERT_TRUE(SUCCEEDED(device._device.As(&queue)));
    EXPECT_TRUE(context.Diagnostics->SelfTest(queue.Get()));
    queue.Reset();
    context.Reset();
    EXPECT_TRUE(lifetime.expired());
}

TEST_F(D3D12DiagnosticsReaders, CallbackImmediatelyDeliversWithoutAnyStorageAccess) {
    using namespace d3d12::detail;
    DiagnosticQueueSpy spy{Queue};
    RecordProperty("diagnostic_callback_runtime", spy.Callback ? "native" : "modeled");
    std::atomic<uint32_t> errors{0};
    auto diagnosticContext = DXGIDiagnosticContext::Create().Unwrap();
    auto probe = make_shared<test::DiagnosticProbe>(errors, diagnosticContext);
    auto subscription = ObserveDiagnostics(diagnosticContext, probe);
    vector<string> log;
    auto session = CreateDiagnosticSession({.LogCallback = [](LogLevel, std::string_view message, void* data) {
                                                static_cast<vector<string>*>(data)->emplace_back(message);
                                            },
                                            .LogUserData = &log,
                                            .DiagnosticContext = diagnosticContext})
                       .Unwrap();
    auto channel = AttachDiagnosticChannel(session, &spy, &spy).Unwrap();
    auto shared = AttachDiagnosticChannel(session, &spy, &spy).Unwrap();
    EXPECT_FALSE(AttachDiagnosticChannel(session, &spy, nullptr));
    ASSERT_EQ(spy.Emit("immediate callback"), S_OK);
    EXPECT_TRUE(probe->HasMarker(DiagnosticSource::D3D12, "immediate callback"));
    EXPECT_TRUE(std::ranges::any_of(log, [](const auto& line) { return line.find("immediate callback") != string::npos; }));
    const auto stored = Queue->GetNumStoredMessages();
    EXPECT_TRUE(FlushDiagnostics(diagnosticContext));
    {
        DiagnosticTopologyScope topology{session};
    }
    channel.reset();
    EXPECT_EQ(spy.Emit("shared callback"), S_OK);
    EXPECT_TRUE(probe->HasMarker(DiagnosticSource::D3D12, "shared callback"));
    shared.reset();
    EXPECT_GE(Queue->GetNumStoredMessages(), stored);
    EXPECT_EQ(spy.StorageCalls, 0u);
    EXPECT_EQ(spy.References, 1u);
    session.reset();
    subscription.reset();
    EXPECT_TRUE(probe->Verify());
}

TEST_F(D3D12DiagnosticsReaders, RegistrationFailureDoesNotFallBack) {
    using namespace d3d12::detail;
    DiagnosticQueueSpy spy{Queue};
    spy.RegisterResult = E_FAIL;
    std::atomic<uint32_t> errors{0};
    auto diagnosticContext = DXGIDiagnosticContext::Create().Unwrap();
    auto probe = make_shared<test::DiagnosticProbe>(errors, diagnosticContext);
    auto subscription = ObserveDiagnostics(diagnosticContext, probe);
    auto session = CreateDiagnosticSession({.LogCallback = [](LogLevel, std::string_view, void*) {}, .DiagnosticContext = diagnosticContext}).Unwrap();
    probe->Expect({.SourceType = DiagnosticSource::D3D12, .Severity = D3D12_MESSAGE_SEVERITY_ERROR, .Fault = DiagnosticFailure::InitializationFailed, .Result = E_FAIL});
    EXPECT_FALSE(AttachDiagnosticChannel(session, &spy, &spy));
    EXPECT_EQ(spy.StorageCalls, 0u);
    EXPECT_EQ(spy.References, 1u);
    session.reset();
    subscription.reset();
    EXPECT_TRUE(probe->Verify());
}

TEST_F(D3D12DiagnosticsReaders, UnregisterWaitsForInFlightDelivery) {
    using namespace d3d12::detail;
    struct BlockingObserver final : DiagnosticObserver {
        std::mutex Mutex;
        std::condition_variable Changed;
        bool Entered{}, Released{}, Unregistering{}, Closed{};
        void OnDiagnostic(const DiagnosticMessage& message) noexcept override {
            if (message.Description != "in-flight callback") return;
            std::unique_lock lock{Mutex};
            Entered = true;
            Changed.notify_all();
            Changed.wait(lock, [&] { return Released; });
        }
    };
    DiagnosticQueueSpy spy{Queue};
    RecordProperty("diagnostic_callback_runtime", spy.Callback ? "native" : "modeled");
    auto diagnosticContext = DXGIDiagnosticContext::Create().Unwrap();
    auto observer = make_shared<BlockingObserver>();
    auto subscription = ObserveDiagnostics(diagnosticContext, observer);
    auto session = CreateDiagnosticSession({.LogCallback = [](LogLevel, std::string_view, void*) {}, .DiagnosticContext = diagnosticContext}).Unwrap();
    auto channel = AttachDiagnosticChannel(session, &spy, &spy).Unwrap();
    spy.BeforeUnregister = [&] {
        std::lock_guard lock{observer->Mutex};
        observer->Unregistering = true;
        observer->Changed.notify_all();
    };
    std::thread sender, closer;
    auto join = MakeScopeGuard([&] {
        {
            std::lock_guard lock{observer->Mutex};
            observer->Released = true;
        }
        observer->Changed.notify_all();
        if (sender.joinable()) sender.join();
        if (closer.joinable()) closer.join();
    });
    std::atomic<HRESULT> result{E_PENDING};
    sender = std::thread([&] { result = spy.Emit("in-flight callback"); });
    {
        std::unique_lock lock{observer->Mutex};
        ASSERT_TRUE(observer->Changed.wait_for(lock, std::chrono::seconds{5}, [&] { return observer->Entered; }));
    }
    closer = std::thread([&] {
        channel.reset();
        std::lock_guard lock{observer->Mutex};
        observer->Closed = true;
        observer->Changed.notify_all();
    });
    {
        std::unique_lock lock{observer->Mutex};
        ASSERT_TRUE(observer->Changed.wait_for(lock, std::chrono::seconds{5}, [&] { return observer->Unregistering; }));
        EXPECT_FALSE(observer->Closed);
        observer->Released = true;
        observer->Changed.notify_all();
        ASSERT_TRUE(observer->Changed.wait_for(lock, std::chrono::seconds{5}, [&] { return observer->Closed; }));
    }
    sender.join();
    closer.join();
    EXPECT_EQ(result.load(), S_OK);
    EXPECT_EQ(spy.StorageCalls, 0u);
}

TEST(D3D12DiagnosticsSmoke, AllQueueTypesAndSlotsFinishBeforeResourcesAreReleased) {
    test::DeviceContext context;
    ASSERT_TRUE(test::TryCreateDevice(RenderBackend::D3D12, context, true, false, {2, 2, 2}));
    vector<unique_ptr<Fence>> fences;
    test::GpuWorkGuard wait{context};
    for (size_t type = 0; type < static_cast<size_t>(QueueType::MAX_COUNT); ++type) {
        for (uint32_t slot = 0; slot < 2; ++slot) {
            auto queue = context.Device->GetCommandQueue(static_cast<QueueType>(type), slot).Unwrap();
            fences.push_back(context.Device->CreateFence().Unwrap());
            Fence* signals[]{fences.back().get()};
            uint64_t values[]{1};
            queue->Submit({.SignalFences = signals, .SignalValues = values});
        }
    }
    context.WaitAllQueues();
    for (const auto& fence : fences) EXPECT_EQ(fence->GetCompletedValue(), 1u);
    fences.clear();
    auto probe = context.Diagnostics;
    context.Reset();
    const auto count = probe->Messages.size();
    context.Reset();
    EXPECT_EQ(probe->Messages.size(), count);
    ASSERT_TRUE(test::TryCreateDevice(RenderBackend::D3D12, context, true, false));
    context.Reset();
}

TEST(D3D12DiagnosticsGbv, AsyncErrorIsDeliveredBeforeFinalization) {
    const char* enabled = std::getenv("RADRAY_TEST_GPU_VALIDATION");
    if (!enabled || std::string_view{enabled} != "1") GTEST_SKIP() << "Requires an isolated --gpu-validation run";
    test::DeviceContext context;
    ASSERT_TRUE(test::TryCreateDevice(RenderBackend::D3D12, context, true, false));
    RecordProperty("evidence_class", "isolated_expected_error");
    const auto path = std::filesystem::path{RADRAY_TEST_SHADER_DIR} / "shader_bytecode/compute.CSMain.dxil.bin";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    ASSERT_TRUE(file);
    ASSERT_GT(file.tellg(), 0);
    vector<byte> bytes(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    ASSERT_TRUE(file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())));
    auto& device = *context.Device;
    auto shader = device.CreateShader({bytes, ShaderBlobCategory::DXIL, ShaderStage::Compute}).Unwrap();
    const BindingDescriptor binding{.BindingId = 1, .Kind = BindingKind::RWStructuredBuffer, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = D3D12BindingDescriptor{GetBindingRegisterClass(BindingKind::RWStructuredBuffer), 2, 0, D3D12BufferPlacement::Table}};
    auto group = device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    const PipelineLayoutGroupDescriptor slot{0, group.get()};
    auto layout = device.CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
    auto pso = device.CreateComputePipelineState({layout.get(), {shader.get(), "CSMain"}}).Unwrap();
    auto output = device.CreateBuffer({.Size = 4, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
    auto parameters = device.CreateShaderParameterSet({group.get()}).Unwrap();
    ASSERT_TRUE(parameters->Set(1, 0, ShaderBufferBinding{output.get(), {0, 4}, 4}));
    ASSERT_TRUE(parameters->FlushWrites());
    auto allocator = device.CreateCommandAllocator(context.Queue).Unwrap();
    auto commands = device.CreateCommandBuffer(allocator.get()).Unwrap();
    commands->Begin();
    const ResourceBarrierDescriptor barrier = BarrierBufferDescriptor{output.get(), BufferState::Undefined, BufferState::CopySource};
    commands->ResourceBarrier(std::span{&barrier, 1});
    auto encoder = commands->BeginComputePass().Unwrap();
    encoder->BindComputePipelineState(pso.get());
    encoder->BindShaderParameterSet(0, parameters.get());
    encoder->Dispatch(1, 1, 1);  // GBV checks the descriptor's incompatible state on the GPU.
    commands->EndComputePass(std::move(encoder));
    commands->End();
    const auto begin = context.Diagnostics->CheckpointAndMark();
    const auto expected = context.Diagnostics->Expect({.SourceType = test::DiagnosticProbe::Source::D3D12,
                                                       .Id = D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_INCOMPATIBLE_RESOURCE_STATE,
                                                       .Severity = D3D12_MESSAGE_SEVERITY_ERROR,
                                                       .Begin = begin});
    CommandBuffer* submit[]{commands.get()};
    test::GpuWorkGuard wait{context};
    context.Queue->Submit({.CmdBuffers = submit});
    context.WaitAllQueues();
    context.Diagnostics->EndExpectation(expected);
    EXPECT_EQ(context.Diagnostics->Count(begin, test::DiagnosticProbe::Source::D3D12,
                                         D3D12_MESSAGE_ID_GPU_BASED_VALIDATION_INCOMPATIBLE_RESOURCE_STATE),
              1u);
    RecordProperty("diagnostic_gbv_async", "passed");
}

TEST_F(D3D12DiagnosticsReaders, DxgiClearAccountsForUnreadRaceAndRetainedArrival) {
    using namespace d3d12::detail;
    ASSERT_TRUE(SUCCEEDED(Dxgi->PushEmptyStorageFilter(DXGI_DEBUG_APP)));
    ASSERT_TRUE(SUCCEEDED(Dxgi->PushEmptyRetrievalFilter(DXGI_DEBUG_APP)));
    auto restore = MakeScopeGuard([&] {
        Dxgi->PopRetrievalFilter(DXGI_DEBUG_APP);
        Dxgi->PopStorageFilter(DXGI_DEBUG_APP);
    });
    ASSERT_TRUE(SUCCEEDED(Dxgi->SetMessageCountLimit(DXGI_DEBUG_APP, 4)));
    for (unsigned scenario = 0; scenario < 2; ++scenario) {
        Dxgi->ClearStoredMessages(DXGI_DEBUG_APP);
        DiagnosticQueueSnapshot before{}, after{};
        ASSERT_EQ(ReadDiagnosticSnapshot(*Dxgi.Get(), DXGI_DEBUG_APP, before), S_OK);
        ASSERT_TRUE(SUCCEEDED(Dxgi->AddApplicationMessage(DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO, "copied")));
        vector<DiagnosticMessage> messages;
        ASSERT_EQ(CopyDiagnosticBatch(*Dxgi.Get(), DXGI_DEBUG_APP, before, messages), S_OK);
        ASSERT_EQ(messages.size(), 1u);
        if (scenario != 0)
            ASSERT_TRUE(SUCCEEDED(Dxgi->AddApplicationMessage(DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO, "clear-race")));
        Dxgi->ClearStoredMessages(DXGI_DEBUG_APP);
        ASSERT_TRUE(SUCCEEDED(Dxgi->AddApplicationMessage(DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO, "retained")));
        ASSERT_EQ(ReadDiagnosticSnapshot(*Dxgi.Get(), DXGI_DEBUG_APP, after), S_OK);
        EXPECT_GE(after.Accepted, before.Accepted);
        EXPECT_EQ(IsDiagnosticClearComplete(before, after), scenario == 0);
        ASSERT_EQ(CopyDiagnosticBatch(*Dxgi.Get(), DXGI_DEBUG_APP, after, messages), S_OK);
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_EQ(messages.front().Description, "retained");
    }
    RecordProperty("diagnostic_algorithm", "clear_race");
    RecordProperty("diagnostic_algorithm_result", "passed");
}

TEST_F(D3D12DiagnosticsReaders, DxgiMultipleFactoriesDoNotFormOneEvictionWindow) {
    using namespace d3d12::detail;
    RecordProperty("evidence_class", "isolated_expected_error");
    RecordProperty("expected_validation_errors", 2);
    d3d12::ComPtr<IDXGIFactory4> otherFactory;
    ASSERT_TRUE(SUCCEEDED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&otherFactory))));
    Dxgi->ClearStoredMessages(DXGI_DEBUG_DXGI);
    ASSERT_TRUE(SUCCEEDED(Dxgi->PushEmptyStorageFilter(DXGI_DEBUG_DXGI)));
    ASSERT_TRUE(SUCCEEDED(Dxgi->PushEmptyRetrievalFilter(DXGI_DEBUG_DXGI)));
    auto restore = MakeScopeGuard([&] {
        Dxgi->PopRetrievalFilter(DXGI_DEBUG_DXGI);
        Dxgi->PopStorageFilter(DXGI_DEBUG_DXGI);
    });
    ASSERT_TRUE(SUCCEEDED(Dxgi->SetMessageCountLimit(DXGI_DEBUG_DXGI, 2)));
    DiagnosticQueueSnapshot before{}, after{};
    ASSERT_EQ(ReadDiagnosticSnapshot(*Dxgi.Get(), DXGI_DEBUG_DXGI, before), S_OK);
    const auto initial = before;
    ASSERT_TRUE(SUCCEEDED(otherFactory->MakeWindowAssociation(reinterpret_cast<HWND>(1), 0)));
    for (unsigned index = 0; index < 5; ++index)
        ASSERT_EQ(Factory->MakeWindowAssociation(nullptr, 0x80000000), DXGI_ERROR_INVALID_CALL);
    vector<DiagnosticMessage> messages;
    ASSERT_EQ(CopyDiagnosticBatch(*Dxgi.Get(), DXGI_DEBUG_DXGI, before, messages), S_OK);
    EXPECT_GE(messages.size(), 2u);
    const auto errors = std::ranges::count_if(messages, [](const auto& message) { return message.Severity == DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR; });
    EXPECT_EQ(errors, 2);
    RecordProperty("observed_validation_errors", static_cast<int>(errors));
    Dxgi->ClearStoredMessages(DXGI_DEBUG_DXGI);
    ASSERT_EQ(ReadDiagnosticSnapshot(*Dxgi.Get(), DXGI_DEBUG_DXGI, after), S_OK);
    EXPECT_GT(after.Discarded, initial.Discarded);
    EXPECT_FALSE(IsDiagnosticAppendComplete(initial, before));
    EXPECT_TRUE(IsDiagnosticClearComplete(before, after));
    RecordProperty("diagnostic_algorithm", "dxgi_multi_queue");
    RecordProperty("diagnostic_algorithm_result", "passed");
}

TEST(D3D12DiagnosticsSmoke, SwapChainTopologyKeepsDxgiAccountingHealthy) {
    test::DeviceContext context;
    ASSERT_TRUE(test::TryCreateDevice(RenderBackend::D3D12, context, true, false));
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"RadRay D3D12 diagnostics",
                                        WS_OVERLAPPEDWINDOW, 0, 0, 192, 144, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(window, nullptr);
    auto destroyWindow = MakeScopeGuard([&] { DestroyWindow(window); });
    SwapChainDescriptor desc{};
    desc.PresentQueue = context.Queue;
    desc.NativeHandler = window;
    desc.Width = 128;
    desc.Height = 96;
    desc.BackBufferCount = 3;
    desc.Format = TextureFormat::BGRA8_UNORM_SRGB;
    auto chain = context.Device->CreateSwapChain(desc);
    ASSERT_TRUE(chain);
    d3d12::ComPtr<ID3D12InfoQueue> messages;
    auto& native = *static_cast<d3d12::DeviceD3D12*>(context.Device.get());
    {
        auto& nativeChain = *static_cast<d3d12::SwapChainD3D12*>(chain.Get());
        for (const auto& frame : nativeChain._frames) {
            EXPECT_EQ(frame.image->_rawDesc.Format, DXGI_FORMAT_B8G8R8A8_UNORM);
            EXPECT_EQ(frame.image->GetDesc().Format, TextureFormat::BGRA8_UNORM_SRGB);
            const TextureViewDescriptor viewDesc{frame.image.get(), TextureDimension::Dim2D, desc.Format, {0, 1, 0, 1}, TextureViewUsage::RenderTarget};
            EXPECT_TRUE(native.ValidateTextureView(viewDesc).first);
            ASSERT_TRUE(context.Device->CreateTextureView(viewDesc));
        }
    }
    ASSERT_EQ(native._device.As(&messages), S_OK);
    ASSERT_TRUE(SUCCEEDED(messages->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "present checkpoint")));
    d3d12::ComPtr<IDXGIInfoQueue> dxgi;
    ASSERT_EQ(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgi)), S_OK);
    ASSERT_EQ(dxgi->AddApplicationMessage(DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO, "present checkpoint"), S_OK);
    // The hidden window returns early, which must still run the checkpoint.
    EXPECT_EQ(chain->Present({}).Status, SwapChainStatus::RetryLater);
    EXPECT_TRUE(context.Diagnostics->HasMarker(test::DiagnosticProbe::Source::D3D12, "present checkpoint"));
    EXPECT_TRUE(context.Diagnostics->HasMarker(test::DiagnosticProbe::Source::Dxgi, "present checkpoint"));
    ASSERT_TRUE(SUCCEEDED(messages->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "wait checkpoint")));
    context.Queue->Wait();
    EXPECT_TRUE(context.Diagnostics->HasMarker(test::DiagnosticProbe::Source::D3D12, "wait checkpoint"));
    messages.Reset();
    dxgi.Reset();
    ASSERT_TRUE(chain->Recreate(160, 120, desc.Format, PresentMode::FIFO));
    chain->Destroy();
    chain->Destroy();
    chain.Release().reset();
    EXPECT_TRUE(context.Diagnostics->Checkpoint());
    context.Reset();
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}

TEST(D3D12DiagnosticsSmoke, SharedChannelSurvivesFirstSessionAndRestarts) {
    testing::Test::RecordProperty("diagnostic_algorithm", "channel_lifecycle");
    auto diagnosticContext = DXGIDiagnosticContext::Create().Unwrap();
    test::DeviceContext first, second;
    ASSERT_TRUE(test::TryCreateDevice(RenderBackend::D3D12, first, true, false, {1, 0, 0}, diagnosticContext));
    ASSERT_TRUE(test::TryCreateDevice(RenderBackend::D3D12, second, true, false, {1, 0, 0}, diagnosticContext));
    first.Reset();
    auto& device = *static_cast<d3d12::DeviceD3D12*>(second.Device.get());
    d3d12::ComPtr<ID3D12InfoQueue> queue;
    ASSERT_TRUE(SUCCEEDED(device._device.As(&queue)));
    EXPECT_TRUE(second.Diagnostics->SelfTest(queue.Get()));
    EXPECT_TRUE(second.Diagnostics->Checkpoint());
    queue.Reset();
    second.Reset();
    EXPECT_EQ(first.ValidationErrors.load(), 0u);
    EXPECT_EQ(second.ValidationErrors.load(), 0u);
    test::DeviceContext restarted;
    ASSERT_TRUE(test::TryCreateDevice(RenderBackend::D3D12, restarted, true, false, {1, 0, 0}, diagnosticContext));
    EXPECT_TRUE(restarted.DiagnosticSelfTest);
    restarted.Reset();
    EXPECT_EQ(restarted.ValidationErrors.load(), 0u);
    testing::Test::RecordProperty("diagnostic_algorithm_result", "passed");
}

}  // namespace
}  // namespace radray::render
#endif
