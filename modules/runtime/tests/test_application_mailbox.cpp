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

// ---------------------------------------------------------------------------
// 以下测试模拟真实调度器的 ring-buffer flight 选择逻辑。
// 真实代码: application.cpp 固定取 _surface->_nextFrameSlotIndex 作为 flight，
// gpu_system.cpp AcquireSwapChainUnlocked 成功 acquire 后才推进 index。
//
// 每个 tick 严格按 ScheduleFramesSingleThreaded 的三阶段执行：
//   Phase 1: CollectCompletedFlightTasks (扫描所有 flight)
//   Phase 2: GetPrepareMailboxSlot → PublishPreparedMailbox
//   Phase 3: Submit — 取 ring 位置的 flight:
//            busy + allowFrameDrop  → skip (ring 不推进)
//            busy + !allowFrameDrop → Wait + 定点回收
//            空闲 → 直接提交
//            成功后 mark InRender, BeginFrame 推进 ring
// ---------------------------------------------------------------------------

// CPU 比 GPU 快 (allowFrameDrop=true)：
// ring 指向的 flight 若仍忙且未完成 → 跳过本 tick 提交, ring 不推进。
// CPU 继续 prepare+publish, 覆盖 Published 槽位, 中间帧被丢弃。
//
// tick | ring | slot | submit? | mailbox state               | note
// -----+------+------+---------+-----------------------------+-----
//  0   | 0→1 |  0   | yes     | [InRender, Free, Free]      |
//  1   | 1→0 |  1   | yes     | [InRender, InRender, Free]  |
//  2   | 0   |  2   | no      | [InR, InR, Published]       | flight 0 busy, skip
//  3   | 0   |  2†  | no      | [InR, InR, Published]       | C→D 覆盖
//  4   | 0→1 |  0   | yes     | [InRender, InR, Free]       | GPU done A; D superseded
//  5   | 1→0 |  1   | yes     | [InR, InRender, Free]       | GPU done B
TEST(ApplicationMailboxTest, CpuFasterThanGpu_DropsIntermediateFrames) {
    AppWindow window{};
    window._mailboxes.resize(3);
    window._flights.resize(2);
    window.ResetMailboxes();

    FakeFence fence0{};
    FakeFence fence1{};
    uint32_t ring = 0;

    // === Tick 0 ===
    window.CollectCompletedFlightTasks();
    {
        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 0u);
        window.PublishPreparedMailbox(*slot);

        // Submit: ring=0, flight 0 empty
        EXPECT_EQ(ring, 0u);
        EXPECT_FALSE(window._flights[ring]._task.has_value());
        auto pub = window.GetPublishedMailboxSlot();
        ASSERT_TRUE(pub.has_value());
        window._mailboxes[*pub]._state = AppWindow::MailboxState::InRender;
        fence0.Signal(1);
        window._flights[ring]._task.emplace(nullptr, &fence0, 1);
        window._flights[ring]._mailboxSlot = *pub;
        ring = (ring + 1) % 2;
    }
    EXPECT_EQ(ring, 1u);
    EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::Free);
    EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::Free);

    // === Tick 1: GPU A 未完成 ===
    window.CollectCompletedFlightTasks();
    EXPECT_TRUE(window._flights[0]._task.has_value());
    {
        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 1u);
        window.PublishPreparedMailbox(*slot);

        // Submit: ring=1, flight 1 empty
        EXPECT_EQ(ring, 1u);
        EXPECT_FALSE(window._flights[ring]._task.has_value());
        auto pub = window.GetPublishedMailboxSlot();
        ASSERT_TRUE(pub.has_value());
        window._mailboxes[*pub]._state = AppWindow::MailboxState::InRender;
        fence1.Signal(1);
        window._flights[ring]._task.emplace(nullptr, &fence1, 1);
        window._flights[ring]._mailboxSlot = *pub;
        ring = (ring + 1) % 2;
    }
    EXPECT_EQ(ring, 0u);
    EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::Free);

    // === Tick 2: ring=0, flight 0 busy & not completed → skip ===
    window.CollectCompletedFlightTasks();
    {
        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 2u);
        window.PublishPreparedMailbox(*slot);

        EXPECT_EQ(ring, 0u);
        EXPECT_TRUE(window._flights[ring]._task.has_value());
        EXPECT_FALSE(window._flights[ring]._task->IsCompleted());
        // allowFrameDrop → skip, ring 不推进
    }
    EXPECT_EQ(ring, 0u);
    EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::Published);
    EXPECT_EQ(window._mailboxes[2]._generation, 3u);

    // === Tick 3: ring=0, flight 0 still busy → skip; 覆盖帧 C ===
    window.CollectCompletedFlightTasks();
    {
        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 2u);                     // no Free, fallback Published
        window.PublishPreparedMailbox(*slot);

        EXPECT_EQ(ring, 0u);
        EXPECT_TRUE(window._flights[ring]._task.has_value());
        EXPECT_FALSE(window._flights[ring]._task->IsCompleted());
    }
    EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::Published);
    EXPECT_EQ(window._mailboxes[2]._generation, 4u);

    // === Tick 4: GPU done A, ring=0 → flight 0 completed by Collect ===
    fence0.Complete(1);
    window.CollectCompletedFlightTasks();
    EXPECT_FALSE(window._flights[0]._task.has_value());
    EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::Free);
    {
        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 0u);
        window.PublishPreparedMailbox(*slot);
        // slot 2 (Published D) superseded → released
    }
    EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::Published);
    EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::Free);
    EXPECT_EQ(window._mailboxes[0]._generation, 5u);
    {
        // Submit: ring=0, flight 0 empty → submit
        EXPECT_EQ(ring, 0u);
        EXPECT_FALSE(window._flights[ring]._task.has_value());
        auto pub = window.GetPublishedMailboxSlot();
        ASSERT_TRUE(pub.has_value());
        EXPECT_EQ(*pub, 0u);
        window._mailboxes[*pub]._state = AppWindow::MailboxState::InRender;
        fence0.Signal(2);
        window._flights[ring]._task.emplace(nullptr, &fence0, 2);
        window._flights[ring]._mailboxSlot = *pub;
        ring = (ring + 1) % 2;
    }
    EXPECT_EQ(ring, 1u);
    EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::Free);

    // === Tick 5: GPU done B ===
    fence1.Complete(1);
    window.CollectCompletedFlightTasks();
    EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::Free);
    {
        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 1u);
        window.PublishPreparedMailbox(*slot);

        // Submit: ring=1, flight 1 empty
        EXPECT_EQ(ring, 1u);
        EXPECT_FALSE(window._flights[ring]._task.has_value());
        auto pub = window.GetPublishedMailboxSlot();
        ASSERT_TRUE(pub.has_value());
        window._mailboxes[*pub]._state = AppWindow::MailboxState::InRender;
        fence1.Signal(2);
        window._flights[ring]._task.emplace(nullptr, &fence1, 2);
        window._flights[ring]._mailboxSlot = *pub;
        ring = (ring + 1) % 2;
    }
    EXPECT_EQ(ring, 0u);
    EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::InRender);  // 帧 E
    EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::InRender);  // 帧 F
    EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::Free);
    // 结论：帧 C D 丢弃, GPU 只渲染 A B E F; ring 因 skip 在 0 停滞后恢复轮转
}

// GPU 比 CPU 快：每 tick GPU 已完成, mailbox 始终复用 slot 0,
// 但 ring 仍然轮转, flight 交替 0→1→0→1。
//
// tick | ring | mailbox slot | flight | mailbox state
// -----+------+--------------+--------+----
//  0   | 0→1 |     0        |   0    | [InRender, Free, Free]
//  1   | 1→0 |     0        |   1    | [InRender, Free, Free]
//  2   | 0→1 |     0        |   0    | [InRender, Free, Free]
//  3   | 1→0 |     0        |   1    | [InRender, Free, Free]
TEST(ApplicationMailboxTest, GpuFasterThanCpu_FlightsAlternateViaRing) {
    AppWindow window{};
    window._mailboxes.resize(3);
    window._flights.resize(2);
    window.ResetMailboxes();

    FakeFence fence{};
    uint32_t ring = 0;

    for (uint64_t tick = 0; tick < 5; ++tick) {
        const uint64_t signalVal = tick + 1;

        if (tick > 0) {
            fence.Complete(signalVal - 1);
        }
        window.CollectCompletedFlightTasks();

        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 0u) << "tick " << tick;       // mailbox 始终 slot 0
        window.PublishPreparedMailbox(*slot);

        // Submit: ring 位置的 flight 一定空闲（上一帧已被 Collect 回收）
        const uint32_t fi = ring;
        const uint32_t expectedFlight = static_cast<uint32_t>(tick % 2);
        EXPECT_EQ(fi, expectedFlight) << "tick " << tick;
        EXPECT_FALSE(window._flights[fi]._task.has_value()) << "tick " << tick;

        auto pub = window.GetPublishedMailboxSlot();
        ASSERT_TRUE(pub.has_value());
        window._mailboxes[*pub]._state = AppWindow::MailboxState::InRender;
        fence.Signal(signalVal);
        window._flights[fi]._task.emplace(nullptr, &fence, signalVal);
        window._flights[fi]._mailboxSlot = *pub;
        ring = (ring + 1) % 2;

        EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::InRender) << "tick " << tick;
        EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::Free) << "tick " << tick;
        EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::Free) << "tick " << tick;
    }
    // 结论：GPU 快时 mailbox 只用 slot 0, 但 flight 因 ring 交替 0/1
}

// 稳态：GPU 延迟 1 帧, ring 每 tick 推进, mailbox 和 flight 都交替 0/1。
// 第三个 mailbox 槽位从未用到。
//
// tick | ring | GPU done | mailbox slot | flight | mailbox state
// -----+------+----------+--------------+--------+---
//  0   | 0→1 |  -       |     0        |   0    | [InRender, Free, Free]
//  1   | 1→0 |  -       |     1        |   1    | [InRender, InRender, Free]
//  2   | 0→1 |  A       |     0        |   0    | [InRender, InRender, Free]
//  3   | 1→0 |  B       |     1        |   1    | [InRender, InRender, Free]
//  4   | 0→1 |  C       |     0        |   0    | [InRender, InRender, Free]
TEST(ApplicationMailboxTest, SteadyState_AlternatesTwoSlotsAndFlights) {
    AppWindow window{};
    window._mailboxes.resize(3);
    window._flights.resize(2);
    window.ResetMailboxes();

    FakeFence fence0{};
    FakeFence fence1{};
    FakeFence* fences[] = {&fence0, &fence1};
    uint32_t ring = 0;

    // === Tick 0: 启动 ===
    window.CollectCompletedFlightTasks();
    {
        auto slot = window.GetPrepareMailboxSlot();
        EXPECT_EQ(*slot, 0u);
        window.PublishPreparedMailbox(*slot);

        EXPECT_EQ(ring, 0u);
        window._mailboxes[*slot]._state = AppWindow::MailboxState::InRender;
        fence0.Signal(1);
        window._flights[ring]._task.emplace(nullptr, &fence0, 1);
        window._flights[ring]._mailboxSlot = *slot;
        ring = (ring + 1) % 2;
    }
    EXPECT_EQ(ring, 1u);

    // === Tick 1: GPU A 未完成 ===
    window.CollectCompletedFlightTasks();
    {
        auto slot = window.GetPrepareMailboxSlot();
        EXPECT_EQ(*slot, 1u);
        window.PublishPreparedMailbox(*slot);

        EXPECT_EQ(ring, 1u);
        window._mailboxes[*slot]._state = AppWindow::MailboxState::InRender;
        fence1.Signal(1);
        window._flights[ring]._task.emplace(nullptr, &fence1, 1);
        window._flights[ring]._mailboxSlot = *slot;
        ring = (ring + 1) % 2;
    }
    EXPECT_EQ(ring, 0u);
    // [InRender, InRender, Free]

    // === Tick 2~4: 稳态循环 ===
    for (uint64_t tick = 2; tick <= 4; ++tick) {
        const uint32_t flightIdx = static_cast<uint32_t>(tick % 2);
        const uint32_t expectedSlot = static_cast<uint32_t>(tick % 2);
        const uint64_t nextSignal = (tick / 2) + 1;

        // GPU 完成 2 帧前的提交
        fences[flightIdx]->Complete(fences[flightIdx]->GetLastSignaledValue());
        window.CollectCompletedFlightTasks();

        EXPECT_EQ(ring, flightIdx) << "tick " << tick;
        EXPECT_FALSE(window._flights[flightIdx]._task.has_value()) << "tick " << tick;
        EXPECT_EQ(window._mailboxes[expectedSlot]._state, AppWindow::MailboxState::Free) << "tick " << tick;

        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, expectedSlot) << "tick " << tick;
        window.PublishPreparedMailbox(*slot);

        auto pub = window.GetPublishedMailboxSlot();
        ASSERT_TRUE(pub.has_value());
        window._mailboxes[*pub]._state = AppWindow::MailboxState::InRender;
        fences[flightIdx]->Signal(nextSignal);
        window._flights[ring]._task.emplace(nullptr, fences[flightIdx], nextSignal);
        window._flights[ring]._mailboxSlot = *pub;
        ring = (ring + 1) % 2;

        // 第三个槽位从未使用
        EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::Free) << "tick " << tick;
    }
    // 结论：稳态下 ring 驱动 mailbox/flight 都交替 0/1, 第 3 个槽位冗余
}

// 所有 flight 占满, !allowFrameDrop → ring 位置的 flight 被 Wait + 定点回收
//
// 真实代码在 submit 阶段对 ring 位置做定点回收，不走 CollectCompletedFlightTasks。
TEST(ApplicationMailboxTest, CpuWaitsWhenAllFlightsBusy) {
    AppWindow window{};
    window._mailboxes.resize(3);
    window._flights.resize(2);
    window.ResetMailboxes();

    FakeFence fence0{};
    FakeFence fence1{};
    uint32_t ring = 0;

    // 快速填满: flight 0 → flight 1
    {
        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 0u);
        window.PublishPreparedMailbox(*slot);
        window._mailboxes[*slot]._state = AppWindow::MailboxState::InRender;
        fence0.Signal(1);
        window._flights[ring]._task.emplace(nullptr, &fence0, 1);
        window._flights[ring]._mailboxSlot = *slot;
        ring = (ring + 1) % 2;
    }
    EXPECT_EQ(ring, 1u);
    {
        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 1u);
        window.PublishPreparedMailbox(*slot);
        window._mailboxes[*slot]._state = AppWindow::MailboxState::InRender;
        fence1.Signal(1);
        window._flights[ring]._task.emplace(nullptr, &fence1, 1);
        window._flights[ring]._mailboxSlot = *slot;
        ring = (ring + 1) % 2;
    }
    EXPECT_EQ(ring, 0u);
    // [InRender, InRender, Free], ring 回到 0

    // Phase 1: Collect — 无完成
    window.CollectCompletedFlightTasks();
    EXPECT_TRUE(window._flights[0]._task.has_value());
    EXPECT_TRUE(window._flights[1]._task.has_value());

    // Phase 2: Prepare + publish 帧 C
    {
        auto slot = window.GetPrepareMailboxSlot();
        ASSERT_TRUE(slot.has_value());
        EXPECT_EQ(*slot, 2u);
        window.PublishPreparedMailbox(*slot);
    }
    // [InRender, InRender, Published]

    // Phase 3: Submit — ring=0, flight 0 busy → !allowFrameDrop → Wait + 定点回收
    {
        auto pub = window.GetPublishedMailboxSlot();
        ASSERT_TRUE(pub.has_value());
        EXPECT_EQ(*pub, 2u);

        EXPECT_EQ(ring, 0u);
        EXPECT_TRUE(window._flights[ring]._task.has_value());

        // Wait 阻塞直到 GPU 完成 flight 0
        window._flights[ring]._task->Wait();
        EXPECT_TRUE(window._flights[ring]._task->IsCompleted());

        // 定点回收 ring 位置 (不是 CollectCompletedFlightTasks)
        ASSERT_TRUE(window._flights[ring]._mailboxSlot.has_value());
        window.ReleaseMailbox(*window._flights[ring]._mailboxSlot);
        window._flights[ring]._mailboxSlot.reset();
        window._flights[ring]._task.reset();

        EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::Free);
        EXPECT_TRUE(window._flights[1]._task.has_value());
        EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::InRender);

        // 现在 flight 0 空闲, 提交帧 C
        window._mailboxes[*pub]._state = AppWindow::MailboxState::InRender;
        fence0.Signal(2);
        window._flights[ring]._task.emplace(nullptr, &fence0, 2);
        window._flights[ring]._mailboxSlot = *pub;
        ring = (ring + 1) % 2;
    }
    EXPECT_EQ(ring, 1u);
    EXPECT_EQ(window._mailboxes[0]._state, AppWindow::MailboxState::Free);
    EXPECT_EQ(window._mailboxes[1]._state, AppWindow::MailboxState::InRender);
    EXPECT_EQ(window._mailboxes[2]._state, AppWindow::MailboxState::InRender);
    // 结论：!allowFrameDrop 下 ring 指定的 flight 被 Wait + 定点回收, 然后继续提交
}

}  // namespace
