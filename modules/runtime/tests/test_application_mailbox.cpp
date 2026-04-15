#include <gtest/gtest.h>

#include <radray/runtime/application.h>

using namespace radray;
using namespace radray::render;

namespace {

class FakeFence final : public Fence {
public:
    bool IsValid() const noexcept override { return true; }

    void Destroy() noexcept override {}

    void SetDebugName(std::string_view name) noexcept override {
        _debugName = name;
    }

    uint64_t GetCompletedValue() const noexcept override { return _completedValue; }

    uint64_t GetLastSignaledValue() const noexcept override { return _lastSignaledValue; }

    void Wait() noexcept override { _completedValue = _lastSignaledValue; }

    void Wait(uint64_t value) noexcept override {
        if (value > _completedValue) {
            _completedValue = value;
        }
        if (value > _lastSignaledValue) {
            _lastSignaledValue = value;
        }
    }

    void Signal(uint64_t value) noexcept {
        if (value > _lastSignaledValue) {
            _lastSignaledValue = value;
        }
    }

    void Complete(uint64_t value) noexcept {
        this->Signal(value);
        if (value > _completedValue) {
            _completedValue = value;
        }
    }

private:
    std::string _debugName{};
    uint64_t _completedValue{0};
    uint64_t _lastSignaledValue{0};
};

TEST(ApplicationMailboxTest, UsesFreeSlotBeforeReplacingPublishedLatestAndRetainsInFlightSlots) {
    AppWindow window{};
    window._mailboxes.resize(3);
    window._flights.resize(2);
    window.ResetMailboxes();

    const auto prepareAndPublish = [&window](uint32_t expectedSlot) -> uint32_t {
        const auto mailboxSlot = window.GetPrepareMailboxSlot();
        EXPECT_TRUE(mailboxSlot.has_value());
        if (!mailboxSlot.has_value()) {
            return expectedSlot;
        }
        EXPECT_EQ(*mailboxSlot, expectedSlot);
        window.PublishPreparedMailbox(*mailboxSlot);

        const auto latestPublished = window.GetPublishedMailboxSlot();
        EXPECT_TRUE(latestPublished.has_value());
        if (latestPublished.has_value()) {
            EXPECT_EQ(*latestPublished, *mailboxSlot);
        }
        return *mailboxSlot;
    };

    FakeFence fence0{};
    FakeFence fence1{};

    const uint32_t slot0 = prepareAndPublish(0);
    window._mailboxes[slot0]._state = AppWindow::MailboxState::InRender;
    fence0.Signal(1);
    window._flights[0]._task.emplace(nullptr, &fence0, 1);
    window._flights[0]._mailboxSlot = slot0;

    const uint32_t slot1 = prepareAndPublish(1);
    window._mailboxes[slot1]._state = AppWindow::MailboxState::InRender;
    fence1.Signal(1);
    window._flights[1]._task.emplace(nullptr, &fence1, 1);
    window._flights[1]._mailboxSlot = slot1;

    const uint32_t slot2 = prepareAndPublish(2);
    EXPECT_EQ(slot2, 2u);
    EXPECT_EQ(window._mailboxes[slot0]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[slot1]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[slot2]._state, AppWindow::MailboxState::Published);

    const auto overwritePublished = window.GetPrepareMailboxSlot();
    ASSERT_TRUE(overwritePublished.has_value());
    EXPECT_EQ(*overwritePublished, slot2);
    window.PublishPreparedMailbox(*overwritePublished);
    EXPECT_EQ(window._mailboxes[slot2]._state, AppWindow::MailboxState::Published);

    fence0.Complete(1);
    window.CollectCompletedFlightTasks();
    EXPECT_FALSE(window._flights[0]._task.has_value());
    EXPECT_FALSE(window._flights[0]._mailboxSlot.has_value());
    EXPECT_EQ(window._mailboxes[slot0]._state, AppWindow::MailboxState::Free);
    EXPECT_TRUE(window._flights[1]._task.has_value());
    EXPECT_TRUE(window._flights[1]._mailboxSlot.has_value());
    EXPECT_EQ(window._mailboxes[slot1]._state, AppWindow::MailboxState::InRender);

    const auto latestBeforePublish = window.GetPublishedMailboxSlot();
    ASSERT_TRUE(latestBeforePublish.has_value());
    EXPECT_EQ(*latestBeforePublish, slot2);

    const auto reuseFreedSlot = window.GetPrepareMailboxSlot();
    ASSERT_TRUE(reuseFreedSlot.has_value());
    EXPECT_EQ(*reuseFreedSlot, slot0);
    window.PublishPreparedMailbox(*reuseFreedSlot);

    const auto latestAfterPublish = window.GetPublishedMailboxSlot();
    ASSERT_TRUE(latestAfterPublish.has_value());
    EXPECT_EQ(*latestAfterPublish, slot0);
    EXPECT_EQ(window._mailboxes[slot0]._state, AppWindow::MailboxState::Published);
    EXPECT_EQ(window._mailboxes[slot2]._state, AppWindow::MailboxState::Free);
    EXPECT_EQ(window._mailboxes[slot1]._state, AppWindow::MailboxState::InRender);
}

}  // namespace
