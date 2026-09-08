#include <gtest/gtest.h>

#include <radray/runtime/frame_submission.h>

namespace radray {
namespace {

TEST(FrameSubmissionLifetime, SuccessfulSubmitReleasesOnlySubmitPhaseCaptures) {
    FrameSubmission receipt{17};
    auto stateOwner = make_shared<int>(1);
    auto gpuOwner = make_shared<int>(2);
    weak_ptr<int> stateLifetime = stateOwner;
    weak_ptr<int> gpuLifetime = gpuOwner;
    uint32_t submitted = 0, completed = 0;
    receipt.OnSubmitted = [owner = std::move(stateOwner), &submitted] {
        EXPECT_EQ(*owner, 1);
        ++submitted;
    };
    receipt.OnCompleted = [owner = std::move(gpuOwner), &completed](bool success) {
        EXPECT_TRUE(success);
        EXPECT_EQ(*owner, 2);
        ++completed;
    };

    ASSERT_TRUE(receipt.Record());
    EXPECT_FALSE(receipt.Submit(18));
    EXPECT_FALSE(stateLifetime.expired());
    EXPECT_FALSE(gpuLifetime.expired());
    EXPECT_EQ(submitted, 0u);
    ASSERT_TRUE(receipt.Submit(17));
    EXPECT_EQ(receipt.Status(), FrameOperationStatus::Submitted);
    EXPECT_EQ(submitted, 1u);
    EXPECT_TRUE(stateLifetime.expired());
    EXPECT_FALSE(gpuLifetime.expired());
    EXPECT_FALSE(receipt.Submit(17));
    receipt.Cancel();
    EXPECT_EQ(receipt.Status(), FrameOperationStatus::Submitted);
    EXPECT_FALSE(gpuLifetime.expired());
    EXPECT_FALSE(receipt.Complete(18, true));
    EXPECT_EQ(completed, 0u);
    ASSERT_TRUE(receipt.Complete(17, true));
    EXPECT_EQ(completed, 1u);
    EXPECT_TRUE(gpuLifetime.expired());
}

TEST(FrameSubmissionLifetime, UnsubmittedCancellationReleasesBothPhasesWithoutSubmitting) {
    FrameSubmission receipt{21};
    auto stateOwner = make_shared<int>(1);
    auto gpuOwner = make_shared<int>(2);
    weak_ptr<int> stateLifetime = stateOwner;
    weak_ptr<int> gpuLifetime = gpuOwner;
    uint32_t submitted = 0, cancelled = 0;
    receipt.OnSubmitted = [owner = std::move(stateOwner), &submitted] {
        EXPECT_EQ(*owner, 1);
        ++submitted;
    };
    receipt.OnCompleted = [owner = std::move(gpuOwner), &cancelled](bool success) {
        EXPECT_FALSE(success);
        EXPECT_EQ(*owner, 2);
        ++cancelled;
    };
    ASSERT_TRUE(receipt.Record());
    receipt.Cancel();
    EXPECT_EQ(receipt.Status(), FrameOperationStatus::Cancelled);
    EXPECT_EQ(submitted, 0u);
    EXPECT_EQ(cancelled, 1u);
    EXPECT_TRUE(stateLifetime.expired());
    EXPECT_TRUE(gpuLifetime.expired());
    receipt.Cancel();
    EXPECT_EQ(cancelled, 1u);
}

TEST(FrameSubmissionLifetime, FailedGpuCompletionReleasesFenceCapturesOnce) {
    FrameSubmission receipt{31};
    auto owner = make_shared<int>(3);
    weak_ptr<int> lifetime = owner;
    uint32_t completed = 0;
    receipt.OnCompleted = [owner = std::move(owner), &completed](bool success) {
        EXPECT_FALSE(success);
        EXPECT_EQ(*owner, 3);
        ++completed;
    };
    ASSERT_TRUE(receipt.Record());
    ASSERT_TRUE(receipt.Submit(31));
    EXPECT_FALSE(lifetime.expired());
    ASSERT_TRUE(receipt.Complete(31, false));
    EXPECT_EQ(receipt.Status(), FrameOperationStatus::Cancelled);
    EXPECT_TRUE(lifetime.expired());
    EXPECT_FALSE(receipt.Complete(31, false));
    EXPECT_EQ(completed, 1u);
}

}  // namespace
}  // namespace radray
