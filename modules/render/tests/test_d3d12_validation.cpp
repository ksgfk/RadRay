#include "gpu_test_fixture.h"

#include <cstdio>
#if defined(RADRAY_ENABLE_D3D12)
#include <dxgidebug.h>
#endif

namespace radray::render::test {

namespace {
#if defined(RADRAY_ENABLE_D3D12)

struct ValidationProbe {
    vector<string> Errors;
    Nullable<ID3D12InfoQueue*> AppendQueue{nullptr};
    string AppendMessage;

    void Record(LogLevel level, std::string_view message) {
        if (level != LogLevel::Err && level != LogLevel::Critical) return;
        Errors.emplace_back(message);
        fmt::print(stderr, "{}\n", message);
        if (AppendQueue) {
            auto* queue = AppendQueue.Get();
            AppendQueue = nullptr;
            EXPECT_TRUE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, AppendMessage.c_str())));
        }
    }

    size_t Count(std::string_view marker) const {
        return static_cast<size_t>(std::count_if(Errors.begin(), Errors.end(), [marker](const auto& message) {
            return message.find(marker) != string::npos;
        }));
    }
};

class D3D12ValidationDrain : public testing::Test {
protected:
    void SetUp() override {
        if (!TryCreateDevice(RenderBackend::D3D12, Context, true)) GTEST_SKIP() << Context.Reason;
        Native = static_cast<d3d12::DeviceD3D12*>(Context.Device.get());
        ASSERT_TRUE(SUCCEEDED(Native.Get()->_device.As(&Queue)));
        if (Native.Get()->_infoQueueCallbackCookie.has_value()) {
            d3d12::ComPtr<ID3D12InfoQueue1> callbackQueue;
            ASSERT_TRUE(SUCCEEDED(Native.Get()->_device.As(&callbackQueue)));
            ASSERT_TRUE(SUCCEEDED(callbackQueue->UnregisterMessageCallback(*Native.Get()->_infoQueueCallbackCookie)));
            Native.Get()->_infoQueueCallbackCookie.reset();
        }
        Queue->ClearStoredMessages();
        Native.Get()->TryDrainValidationMessages();
        Native.Get()->_logCallback = +[](LogLevel level, std::string_view message, void* userData) {
            static_cast<ValidationProbe*>(userData)->Record(level, message);
        };
        Native.Get()->_logUserData = &Probe;
    }

    void TearDown() override {
        if (Queue) Queue->ClearRetrievalFilter();
        Context.Reset();
    }

    ValidationProbe Probe;
    DeviceContext Context;
    Nullable<d3d12::DeviceD3D12*> Native{nullptr};
    d3d12::ComPtr<ID3D12InfoQueue> Queue;
};
#else
class D3D12ValidationDrain : public testing::Test {
protected:
    void SetUp() override { GTEST_SKIP() << "D3D12 backend was not built"; }
};
#endif

TEST_F(D3D12ValidationDrain, CloseFailureReportsTheInvalidReadbackBarrierBeforeExit) {
#if defined(RADRAY_ENABLE_D3D12)
    EXPECT_EXIT(([&] {
                    auto readback = Context.Device->CreateBuffer({32, MemoryType::ReadBack, BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
                    auto allocator = Context.Device->CreateCommandAllocator(Context.Queue).Unwrap();
                    auto command = Context.Device->CreateCommandBuffer(allocator.get()).Unwrap();
                    command->Begin();
                    const ResourceBarrierDescriptor invalid = BarrierBufferDescriptor{readback.get(), BufferState::Undefined, BufferState::CopyDestination};
                    command->ResourceBarrier(std::span{&invalid, 1});
                    command->End();
                    std::_Exit(0);
                }()),
                testing::ExitedWithCode(3), "READBACK");
#endif
}

TEST_F(D3D12ValidationDrain, RetrievalFilterControlsTheMessageIndexRange) {
#if defined(RADRAY_ENABLE_D3D12)
    D3D12_MESSAGE_SEVERITY denied = D3D12_MESSAGE_SEVERITY_WARNING;
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumSeverities = 1;
    filter.DenyList.pSeverityList = &denied;
    ASSERT_TRUE(SUCCEEDED(Queue->AddRetrievalFilterEntries(&filter)));
    ASSERT_TRUE(SUCCEEDED(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING, "filtered-readback-probe")));
    ASSERT_TRUE(SUCCEEDED(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "visible-readback-probe")));
    Native.Get()->TryDrainValidationMessages();
    EXPECT_EQ(Probe.Errors.size(), 1u);
    EXPECT_EQ(Probe.Count("visible-readback-probe"), 1u);
    EXPECT_EQ(Queue->GetNumStoredMessages(), 2u);
#endif
}

TEST_F(D3D12ValidationDrain, MessagesAppendedDuringDrainRemainAvailableWithoutDuplicateDelivery) {
#if defined(RADRAY_ENABLE_D3D12)
    Probe.AppendQueue = Queue.Get();
    Probe.AppendMessage = "appended-during-drain-probe";
    ASSERT_TRUE(SUCCEEDED(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "first-drain-probe")));
    Native.Get()->TryDrainValidationMessages();
    EXPECT_EQ(Probe.Count("first-drain-probe"), 1u);
    EXPECT_EQ(Probe.Count("appended-during-drain-probe"), 0u);
    Native.Get()->TryDrainValidationMessages();
    Native.Get()->TryDrainValidationMessages();
    EXPECT_EQ(Probe.Count("first-drain-probe"), 1u);
    EXPECT_EQ(Probe.Count("appended-during-drain-probe"), 1u);
    EXPECT_EQ(Probe.Errors.size(), 2u);
#endif
}

TEST_F(D3D12ValidationDrain, CompletionPollingCollectsMessagesWithoutQueueWait) {
#if defined(RADRAY_ENABLE_D3D12)
    auto fence = Context.Device->CreateFence().Unwrap();
    auto* nativeFence = static_cast<d3d12::FenceD3D12*>(fence.get());
    auto* nativeQueue = static_cast<d3d12::CmdQueueD3D12*>(Context.Queue);
    auto event = d3d12::Win32Event::Create();
    ASSERT_TRUE(event);
    ASSERT_TRUE(SUCCEEDED(nativeQueue->_queue->Signal(nativeFence->_fence.Get(), 1)));
    ASSERT_TRUE(SUCCEEDED(nativeFence->_fence->SetEventOnCompletion(1, event->Get())));
    ASSERT_EQ(WaitForSingleObject(event->Get(), 5000), WAIT_OBJECT_0);
    ASSERT_TRUE(SUCCEEDED(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "completed-fence-probe")));
    EXPECT_EQ(Probe.Count("completed-fence-probe"), 0u);
    EXPECT_EQ(fence->GetCompletedValue(), 1u);
    EXPECT_EQ(Probe.Count("completed-fence-probe"), 1u);
    EXPECT_TRUE(fence->WaitFor(1, 0));
    EXPECT_EQ(Probe.Count("completed-fence-probe"), 1u);
    ASSERT_TRUE(SUCCEEDED(Queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "completed-wait-probe")));
    EXPECT_TRUE(fence->WaitFor(1, 0));
    EXPECT_EQ(Probe.Count("completed-wait-probe"), 1u);
#endif
}

TEST_F(D3D12ValidationDrain, DxgiMessagesUseTheSameLogSink) {
#if defined(RADRAY_ENABLE_D3D12)
    d3d12::ComPtr<IDXGIInfoQueue> queue;
    if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(queue.GetAddressOf())))) GTEST_SKIP() << "DXGI debug queue unavailable";
    ASSERT_TRUE(SUCCEEDED(queue->AddMessage(DXGI_DEBUG_DXGI, DXGI_INFO_QUEUE_MESSAGE_CATEGORY_MISCELLANEOUS,
                                            DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, 0, "dxgi-drain-probe")));
    Native.Get()->TryDrainValidationMessages();
    EXPECT_EQ(Probe.Count("dxgi-drain-probe"), 1u);
    Native.Get()->TryDrainValidationMessages();
    EXPECT_EQ(Probe.Count("dxgi-drain-probe"), 1u);
#endif
}

TEST_F(D3D12ValidationDrain, NewDevicesDoNotReplayAnotherDevicesDxgiHistory) {
#if defined(RADRAY_ENABLE_D3D12)
    d3d12::ComPtr<IDXGIInfoQueue> queue;
    if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(queue.GetAddressOf())))) GTEST_SKIP() << "DXGI debug queue unavailable";
    ASSERT_TRUE(SUCCEEDED(queue->AddMessage(DXGI_DEBUG_DXGI, DXGI_INFO_QUEUE_MESSAGE_CATEGORY_MISCELLANEOUS,
                                            DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, 0, "prior-device-dxgi-probe")));
    Native.Get()->TryDrainValidationMessages();
    EXPECT_EQ(Probe.Count("prior-device-dxgi-probe"), 1u);
    DeviceContext second;
    ASSERT_TRUE(TryCreateDevice(RenderBackend::D3D12, second, true));
    second.Reset();
    EXPECT_EQ(second.ValidationErrors.load(), 0u);
#endif
}

TEST_F(D3D12ValidationDrain, FailedPipelineCreationReportsNativeValidationBeforeReturningEmpty) {
#if defined(RADRAY_ENABLE_D3D12)
    auto layout = Native.Get()->CreatePipelineLayout(ResolvedD3D12Layout{}).Unwrap();
    const array<byte, 4> invalidBytecode{};
    auto shader = Context.Device->CreateShader({invalidBytecode, ShaderBlobCategory::DXIL, ShaderStage::Compute}).Unwrap();
    const auto errorsBefore = Probe.Errors.size();
    auto pipeline = Context.Device->CreateComputePipelineState({layout.get(), {shader.get(), "CSMain"}});
    EXPECT_FALSE(pipeline);
    EXPECT_GT(Probe.Errors.size(), errorsBefore);
#endif
}

}  // namespace

}  // namespace radray::render::test
