#include <gtest/gtest.h>

#include <utility>

#include <radray/scope_guard.h>

using namespace radray;

TEST(ScopeGuardTest, RunsOnScopeExit) {
    int calls = 0;
    {
        auto guard = MakeScopeGuard([&calls]() noexcept { ++calls; });
        EXPECT_EQ(calls, 0);
    }

    EXPECT_EQ(calls, 1);
}

TEST(ScopeGuardTest, DismissSkipsCall) {
    int calls = 0;
    {
        auto guard = MakeScopeGuard([&calls]() noexcept { ++calls; });
        guard.Dismiss();
    }

    EXPECT_EQ(calls, 0);
}

TEST(ScopeGuardTest, MoveTransfersResponsibilityOnce) {
    int calls = 0;
    {
        auto source = MakeScopeGuard([&calls]() noexcept { ++calls; });
        {
            auto target = std::move(source);
        }
        EXPECT_EQ(calls, 1);
    }

    EXPECT_EQ(calls, 1);
}
