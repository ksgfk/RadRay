#include <gtest/gtest.h>

#include <stdexcept>
#include <type_traits>

#include <stdexec/execution.hpp>

#include <radray/basic_corotinue.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {

static_assert(!std::is_copy_constructible_v<RenderFrame>);
static_assert(!std::is_copy_assignable_v<RenderFrame>);
static_assert(std::is_move_constructible_v<RenderFrame>);
static_assert(std::is_copy_constructible_v<RenderFrameTicket>);
static_assert(std::is_copy_assignable_v<RenderFrameTicket>);

task<void> AwaitFrameTicket(
    RenderFrameTicket ticket,
    bool* resumed,
    bool* releaseObserved) {
    co_await ticket;
    *releaseObserved = ticket.IsComplete();
    *resumed = true;
    co_return;
}

TEST(RenderFrame, AutoCancelCleansUpFrameScopedState) {
    RenderService service{};

    bool released = false;
    {
        auto frame = service.BeginFrame();
        EXPECT_TRUE(frame.IsValid());

        void* transient = frame.AllocateTransient(128);
        EXPECT_NE(transient, nullptr);

        frame.DeferRelease([&]() { released = true; });
        EXPECT_EQ(frame.GetStats().DeferredReleaseCount, 1u);
        EXPECT_EQ(frame.GetStats().TransientBytesAllocated, 128u);
    }

    EXPECT_TRUE(released);
}

TEST(RenderFrameTicket, SubmitOrderControlsRetirementOrder) {
    RenderService service{};

    bool firstReady = false;
    bool firstReleased = false;
    auto first = service.BeginFrame();
    first.SetCompletionHooks(RenderFrameCompletionHooks{
        .IsReady = [&]() { return firstReady; },
    });
    first.DeferRelease([&]() { firstReleased = true; });
    auto firstTicket = first.Submit();

    bool secondReleased = false;
    auto second = service.BeginFrame();
    second.DeferRelease([&]() { secondReleased = true; });
    auto secondTicket = second.Submit();

    service.CollectRetiredFrames();

    EXPECT_FALSE(firstTicket.IsComplete());
    EXPECT_FALSE(secondTicket.IsComplete());
    EXPECT_FALSE(firstReleased);
    EXPECT_FALSE(secondReleased);

    firstReady = true;
    service.CollectRetiredFrames();

    EXPECT_TRUE(firstTicket.IsComplete());
    EXPECT_TRUE(secondTicket.IsComplete());
    EXPECT_TRUE(firstReleased);
    EXPECT_TRUE(secondReleased);
}

TEST(RenderFrameTicket, WaitRethrowsCompletionErrorAfterRetirement) {
    RenderService service{};

    bool ready = false;
    bool released = false;
    auto frame = service.BeginFrame();
    frame.DeferRelease([&]() { released = true; });
    frame.SetCompletionHooks(RenderFrameCompletionHooks{
        .IsReady = [&]() { return ready; },
        .Wait = [&]() { ready = true; },
        .GetError = []() {
            return std::make_exception_ptr(std::runtime_error("frame submission failed"));
        },
    });

    auto ticket = frame.Submit();
    EXPECT_THROW(ticket.Wait(), std::runtime_error);
    EXPECT_TRUE(ticket.IsComplete());
    EXPECT_TRUE(ticket.HasError());
    EXPECT_NE(ticket.GetError(), nullptr);
    EXPECT_TRUE(released);
}

TEST(RenderFrameTicket, SenderCompletesAfterFrameRetires) {
    RenderService service{};

    bool released = false;
    auto frame = service.BeginFrame();
    frame.DeferRelease([&]() { released = true; });

    auto ticket = frame.Submit();
    auto result = STDEXEC::sync_wait(ticket.AsSender());

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(ticket.IsComplete());
    EXPECT_TRUE(released);
}

TEST(RenderFrameTicket, CanAwaitTicketInsideTask) {
    RenderService service{};

    bool released = false;
    auto frame = service.BeginFrame();
    frame.DeferRelease([&]() { released = true; });
    auto ticket = frame.Submit();

    bool resumed = false;
    bool releaseObserved = false;
    auto result = STDEXEC::sync_wait(AwaitFrameTicket(ticket, &resumed, &releaseObserved));

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(resumed);
    EXPECT_TRUE(releaseObserved);
    EXPECT_TRUE(released);
}

TEST(RenderService, WaitIdleRetiresAllSubmittedFrames) {
    RenderService service{};

    bool firstReady = false;
    auto first = service.BeginFrame();
    first.SetCompletionHooks(RenderFrameCompletionHooks{
        .IsReady = [&]() { return firstReady; },
        .Wait = [&]() { firstReady = true; },
    });
    auto firstTicket = first.Submit();

    bool secondReady = false;
    auto second = service.BeginFrame();
    second.SetCompletionHooks(RenderFrameCompletionHooks{
        .IsReady = [&]() { return secondReady; },
        .Wait = [&]() { secondReady = true; },
    });
    auto secondTicket = second.Submit();

    service.WaitIdle();

    EXPECT_TRUE(firstTicket.IsComplete());
    EXPECT_TRUE(secondTicket.IsComplete());
}

}  // namespace
}  // namespace radray
