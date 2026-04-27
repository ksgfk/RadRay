#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <gtest/gtest.h>

#include <radray/runtime/application.h>

namespace radray {
namespace {

class MockApplication final : public Application {
public:
    void OnInitialize() override {}
    void OnShutdown() noexcept override {}
    void OnUpdate() override {}
    void OnPrepareRender(AppWindowHandle, uint32_t) override {}
    void OnRender(AppWindowHandle, GpuFrameContext*, uint32_t) override {}
};

enum class ThrowSite {
    Initialize,
    Update
};

class ThrowingApplication final : public Application {
public:
    explicit ThrowingApplication(ThrowSite throwSite) noexcept
        : _throwSite(throwSite) {}

    void OnInitialize() override {
        InitializeCalled = true;
        if (_throwSite == ThrowSite::Initialize) {
            throw std::runtime_error("initialize failed");
        }
    }

    void OnShutdown() noexcept override {
        ShutdownCalled = true;
    }

    void OnUpdate() override {
        UpdateCalled = true;
        if (_throwSite == ThrowSite::Update) {
            throw std::runtime_error("update failed");
        }
        _exitRequested = true;
    }

    void OnPrepareRender(AppWindowHandle, uint32_t) override {}
    void OnRender(AppWindowHandle, GpuFrameContext*, uint32_t) override {}

    bool InitializeCalled{false};
    bool UpdateCalled{false};
    bool ShutdownCalled{false};

private:
    ThrowSite _throwSite{ThrowSite::Initialize};
};

class FakeFence final : public render::Fence {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}

    uint64_t GetCompletedValue() const noexcept override { return _completedValue; }
    uint64_t GetLastSignaledValue() const noexcept override { return _lastSignaledValue; }

    void Wait() noexcept override { _completedValue = _lastSignaledValue; }

    void Wait(uint64_t value) noexcept override {
        if (_completedValue < value) {
            _completedValue = value;
        }
    }

    void Signal(uint64_t value) noexcept {
        _lastSignaledValue = value;
    }

    void Complete(uint64_t value) noexcept {
        if (_completedValue < value) {
            _completedValue = value;
        }
    }

private:
    uint64_t _completedValue{0};
    uint64_t _lastSignaledValue{0};
};

class AppWindowMailboxTest : public ::testing::Test {
protected:
    using MailboxState = AppWindow::MailboxState;
    using FlightState = AppWindow::FlightState;

    void SetUp() override {
        App._multiThreaded = false;

        Window._app = &App;
        Window._surface = make_unique<GpuSurface>(nullptr, unique_ptr<render::SwapChain>{}, 0);
        Window._surface->_frameSlots.resize(2);
        Window._surface->_nextFrameSlotIndex = 0;
        Window._flights.resize(2);
        Window._mailboxes.resize(3);
        Window._channel._queueCapacity = Window._flights.size();
    }

    void TearDown() override {
        Window._channel._queue.clear();
        Window._latestPublished.reset();
        for (AppWindow::FlightData& flight : Window._flights) {
            flight._state = FlightState::Free;
            flight._mailboxSlot = 0;
            flight._mailboxGeneration = 0;
            flight._task = {};
        }
        for (AppWindow::MailboxData& mailbox : Window._mailboxes) {
            mailbox._state = MailboxState::Free;
        }
    }

    void SetNextFlightSlot(uint32_t slot) {
        Window._surface->_nextFrameSlotIndex = slot;
    }

    std::optional<AppWindow::RenderRequest> PublishAndQueueOnFlightSlot(uint32_t flightSlot) {
        auto mailboxSlot = Window.AllocMailboxSlot();
        if (!mailboxSlot.has_value()) {
            return std::nullopt;
        }
        Window.PublishPreparedMailbox(*mailboxSlot);

        this->SetNextFlightSlot(flightSlot);
        return Window.TryQueueLatestPublished();
    }

    std::optional<AppWindow::RenderRequest> QueueClaimAndEnd(uint32_t flightSlot, GpuTask task) {
        auto queued = this->PublishAndQueueOnFlightSlot(flightSlot);
        if (!queued.has_value()) {
            return std::nullopt;
        }

        auto claimed = Window.TryClaimQueuedRenderRequest();
        if (!claimed.has_value()) {
            return std::nullopt;
        }

        Window.EndPrepareRenderTask(*claimed, std::move(task));
        return *claimed;
    }

    MockApplication App;
    AppWindow Window;
};

TEST(ApplicationExceptionTest, RunReturnsNonZeroAndShutsDownWhenInitializeThrows) {
    ThrowingApplication app{ThrowSite::Initialize};

    EXPECT_NE(app.Run(0, nullptr), 0);
    EXPECT_TRUE(app.InitializeCalled);
    EXPECT_FALSE(app.UpdateCalled);
    EXPECT_TRUE(app.ShutdownCalled);
}

TEST(ApplicationExceptionTest, RunReturnsNonZeroAndShutsDownWhenUpdateThrows) {
    ThrowingApplication app{ThrowSite::Update};

    EXPECT_NE(app.Run(0, nullptr), 0);
    EXPECT_TRUE(app.InitializeCalled);
    EXPECT_TRUE(app.UpdateCalled);
    EXPECT_TRUE(app.ShutdownCalled);
}

TEST_F(AppWindowMailboxTest, PublishReplacesOlderLatestAndAllocReusesLatest) {
    auto slot0 = Window.AllocMailboxSlot();
    ASSERT_TRUE(slot0.has_value());
    EXPECT_EQ(*slot0, 0u);
    EXPECT_EQ(Window._mailboxes[*slot0]._state, MailboxState::Preparing);
    EXPECT_EQ(Window._mailboxes[*slot0]._generation, 1u);
    EXPECT_FALSE(Window._latestPublished.has_value());

    auto slot1 = Window.AllocMailboxSlot();
    ASSERT_TRUE(slot1.has_value());
    EXPECT_EQ(*slot1, 1u);
    EXPECT_EQ(Window._mailboxes[*slot1]._state, MailboxState::Preparing);

    Window.PublishPreparedMailbox(*slot0);
    ASSERT_TRUE(Window._latestPublished.has_value());
    EXPECT_EQ(Window._latestPublished->Slot, *slot0);
    EXPECT_EQ(Window._latestPublished->Generation, 1u);
    EXPECT_EQ(Window._mailboxes[*slot0]._state, MailboxState::Published);

    Window.PublishPreparedMailbox(*slot1);
    ASSERT_TRUE(Window._latestPublished.has_value());
    EXPECT_EQ(Window._latestPublished->Slot, *slot1);
    EXPECT_EQ(Window._latestPublished->Generation, 1u);
    EXPECT_EQ(Window._mailboxes[*slot0]._state, MailboxState::Free);
    EXPECT_EQ(Window._mailboxes[*slot1]._state, MailboxState::Published);

    auto reusedLatest = Window.AllocMailboxSlot();
    ASSERT_TRUE(reusedLatest.has_value());
    EXPECT_EQ(*reusedLatest, *slot1);
    EXPECT_FALSE(Window._latestPublished.has_value());
    EXPECT_EQ(Window._mailboxes[*slot1]._state, MailboxState::Preparing);
    EXPECT_EQ(Window._mailboxes[*slot1]._generation, 2u);
}

TEST_F(AppWindowMailboxTest, QueueLatestPublishedUsesSurfaceFrameSlot) {
    auto mailboxSlot = Window.AllocMailboxSlot();
    ASSERT_TRUE(mailboxSlot.has_value());
    Window.PublishPreparedMailbox(*mailboxSlot);

    this->SetNextFlightSlot(1);
    auto request = Window.TryQueueLatestPublished();

    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->FlightSlot, 1u);
    EXPECT_EQ(request->MailboxSlot, *mailboxSlot);
    EXPECT_EQ(request->Generation, 1u);
    EXPECT_FALSE(Window._latestPublished.has_value());
    ASSERT_EQ(Window._channel._queue.size(), 1u);
    EXPECT_EQ(Window._channel._queue.front().FlightSlot, 1u);
    EXPECT_EQ(Window._mailboxes[*mailboxSlot]._state, MailboxState::Queued);

    const AppWindow::FlightData& flight = Window._flights[1];
    EXPECT_EQ(flight._state, FlightState::Queued);
    EXPECT_EQ(flight._mailboxSlot, *mailboxSlot);
    EXPECT_EQ(flight._mailboxGeneration, 1u);

    auto claimed = Window.TryClaimQueuedRenderRequest();
    ASSERT_TRUE(claimed.has_value());
    Window.ReleaseMailbox(*claimed);
}

TEST_F(AppWindowMailboxTest, BusyFlightSlotKeepsLatestPublishedAvailable) {
    auto mailboxSlot = Window.AllocMailboxSlot();
    ASSERT_TRUE(mailboxSlot.has_value());
    Window.PublishPreparedMailbox(*mailboxSlot);

    this->SetNextFlightSlot(1);
    Window._flights[1]._state = FlightState::Queued;

    auto request = Window.TryQueueLatestPublished();

    EXPECT_FALSE(request.has_value());
    ASSERT_TRUE(Window._latestPublished.has_value());
    EXPECT_EQ(Window._latestPublished->Slot, *mailboxSlot);
    EXPECT_EQ(Window._mailboxes[*mailboxSlot]._state, MailboxState::Published);
    EXPECT_TRUE(Window._channel._queue.empty());
}

TEST_F(AppWindowMailboxTest, ClaimQueuedRequestMovesMailboxAndFlightIntoPrepare) {
    auto queued = this->PublishAndQueueOnFlightSlot(0);
    ASSERT_TRUE(queued.has_value());

    auto claimed = Window.TryClaimQueuedRenderRequest();

    ASSERT_TRUE(claimed.has_value());
    EXPECT_EQ(claimed->FlightSlot, queued->FlightSlot);
    EXPECT_EQ(claimed->MailboxSlot, queued->MailboxSlot);
    EXPECT_EQ(claimed->Generation, queued->Generation);
    EXPECT_TRUE(Window._channel._queue.empty());
    EXPECT_EQ(Window._mailboxes[queued->MailboxSlot]._state, MailboxState::InRender);
    EXPECT_EQ(Window._flights[queued->FlightSlot]._state, FlightState::Preparing);

    Window.ReleaseMailbox(*claimed);
    EXPECT_EQ(Window._mailboxes[queued->MailboxSlot]._state, MailboxState::Free);
    EXPECT_EQ(Window._flights[queued->FlightSlot]._state, FlightState::Free);
}

TEST_F(AppWindowMailboxTest, EndPrepareAndCollectCompletedTaskFreesSlot) {
    auto queued = this->PublishAndQueueOnFlightSlot(0);
    ASSERT_TRUE(queued.has_value());
    auto claimed = Window.TryClaimQueuedRenderRequest();
    ASSERT_TRUE(claimed.has_value());

    Window.EndPrepareRenderTask(*claimed, GpuTask{});
    EXPECT_EQ(Window._mailboxes[queued->MailboxSlot]._state, MailboxState::InRender);
    EXPECT_EQ(Window._flights[queued->FlightSlot]._state, FlightState::InRender);

    Window.CollectCompletedFlightSlots();
    EXPECT_EQ(Window._mailboxes[queued->MailboxSlot]._state, MailboxState::Free);
    EXPECT_EQ(Window._flights[queued->FlightSlot]._state, FlightState::Free);
    EXPECT_EQ(Window._flights[queued->FlightSlot]._mailboxSlot, 0u);
    EXPECT_EQ(Window._flights[queued->FlightSlot]._mailboxGeneration, 0u);
}

TEST_F(AppWindowMailboxTest, CpuFasterThanGpuKeepsOnlyLatestPublishedWhileFlightsBusy) {
    FakeFence fence;
    fence.Signal(3);

    auto frame0 = this->QueueClaimAndEnd(0, GpuTask{nullptr, &fence, 1});
    ASSERT_TRUE(frame0.has_value());
    auto frame1 = this->QueueClaimAndEnd(1, GpuTask{nullptr, &fence, 2});
    ASSERT_TRUE(frame1.has_value());
    EXPECT_EQ(Window._flights[0]._state, FlightState::InRender);
    EXPECT_EQ(Window._flights[1]._state, FlightState::InRender);
    EXPECT_EQ(Window._mailboxes[frame0->MailboxSlot]._state, MailboxState::InRender);
    EXPECT_EQ(Window._mailboxes[frame1->MailboxSlot]._state, MailboxState::InRender);

    auto frame2Mailbox = Window.AllocMailboxSlot();
    ASSERT_TRUE(frame2Mailbox.has_value());
    Window.PublishPreparedMailbox(*frame2Mailbox);
    this->SetNextFlightSlot(0);
    EXPECT_FALSE(Window.TryQueueLatestPublished().has_value());
    ASSERT_TRUE(Window._latestPublished.has_value());
    EXPECT_EQ(Window._latestPublished->Slot, *frame2Mailbox);
    EXPECT_EQ(Window._latestPublished->Generation, 1u);
    EXPECT_EQ(Window._mailboxes[*frame2Mailbox]._state, MailboxState::Published);

    auto frame3Mailbox = Window.AllocMailboxSlot();
    ASSERT_TRUE(frame3Mailbox.has_value());
    EXPECT_EQ(*frame3Mailbox, *frame2Mailbox);
    Window.PublishPreparedMailbox(*frame3Mailbox);
    EXPECT_FALSE(Window.TryQueueLatestPublished().has_value());
    ASSERT_TRUE(Window._latestPublished.has_value());
    EXPECT_EQ(Window._latestPublished->Slot, *frame3Mailbox);
    EXPECT_EQ(Window._latestPublished->Generation, 2u);
    EXPECT_EQ(Window._mailboxes[*frame3Mailbox]._state, MailboxState::Published);
    EXPECT_TRUE(Window._channel._queue.empty());

    fence.Complete(1);
    Window.CollectCompletedFlightSlots();
    EXPECT_EQ(Window._flights[0]._state, FlightState::Free);
    EXPECT_EQ(Window._mailboxes[frame0->MailboxSlot]._state, MailboxState::Free);
    EXPECT_EQ(Window._flights[1]._state, FlightState::InRender);

    auto queuedLatest = Window.TryQueueLatestPublished();
    ASSERT_TRUE(queuedLatest.has_value());
    EXPECT_EQ(queuedLatest->FlightSlot, 0u);
    EXPECT_EQ(queuedLatest->MailboxSlot, *frame3Mailbox);
    EXPECT_EQ(queuedLatest->Generation, 2u);
    EXPECT_EQ(Window._mailboxes[*frame3Mailbox]._state, MailboxState::Queued);
    EXPECT_EQ(Window._flights[0]._state, FlightState::Queued);

    auto claimedLatest = Window.TryClaimQueuedRenderRequest();
    ASSERT_TRUE(claimedLatest.has_value());
    Window.EndPrepareRenderTask(*claimedLatest, GpuTask{nullptr, &fence, 3});
    EXPECT_EQ(Window._mailboxes[*frame3Mailbox]._state, MailboxState::InRender);
    EXPECT_EQ(Window._flights[0]._state, FlightState::InRender);

    fence.Complete(3);
    Window.CollectCompletedFlightSlots();
    EXPECT_EQ(Window._flights[0]._state, FlightState::Free);
    EXPECT_EQ(Window._flights[1]._state, FlightState::Free);
    EXPECT_EQ(Window._mailboxes[frame1->MailboxSlot]._state, MailboxState::Free);
    EXPECT_EQ(Window._mailboxes[*frame3Mailbox]._state, MailboxState::Free);
}

TEST_F(AppWindowMailboxTest, GpuFasterThanCpuReusesFlightsBySurfaceRing) {
    for (uint32_t frameIndex = 0; frameIndex < 4; ++frameIndex) {
        Window.CollectCompletedFlightSlots();

        const uint32_t expectedFlightSlot = frameIndex % 2;
        auto mailboxSlot = Window.AllocMailboxSlot();
        ASSERT_TRUE(mailboxSlot.has_value());
        Window.PublishPreparedMailbox(*mailboxSlot);

        this->SetNextFlightSlot(expectedFlightSlot);
        auto queued = Window.TryQueueLatestPublished();
        ASSERT_TRUE(queued.has_value());
        EXPECT_EQ(queued->FlightSlot, expectedFlightSlot);
        EXPECT_EQ(queued->MailboxSlot, *mailboxSlot);
        EXPECT_EQ(Window._channel._queue.size(), 1u);

        auto claimed = Window.TryClaimQueuedRenderRequest();
        ASSERT_TRUE(claimed.has_value());
        Window.EndPrepareRenderTask(*claimed, GpuTask{});
        EXPECT_EQ(Window._mailboxes[*mailboxSlot]._state, MailboxState::InRender);
        EXPECT_EQ(Window._flights[expectedFlightSlot]._state, FlightState::InRender);

        Window.CollectCompletedFlightSlots();
        EXPECT_EQ(Window._mailboxes[*mailboxSlot]._state, MailboxState::Free);
        EXPECT_EQ(Window._flights[expectedFlightSlot]._state, FlightState::Free);
        EXPECT_FALSE(Window._latestPublished.has_value());
        EXPECT_TRUE(Window._channel._queue.empty());
    }
}

}  // namespace
}  // namespace radray
