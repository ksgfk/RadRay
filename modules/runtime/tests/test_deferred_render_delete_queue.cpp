#include <gtest/gtest.h>

#include <radray/runtime/gpu_system.h>

using namespace radray;

namespace {

class FakeRenderResource final : public render::RenderBase {
public:
    explicit FakeRenderResource(int* destroyCount) noexcept : _destroyCount(destroyCount) {}
    ~FakeRenderResource() noexcept override {
        if (_destroyCount != nullptr) {
            ++*_destroyCount;
        }
    }

    render::RenderObjectTags GetTag() const noexcept override { return render::RenderObjectTag::UNKNOWN; }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}

private:
    int* _destroyCount;
};

}  // namespace

TEST(DeferredRenderDeleteQueueTest, KeepsObjectUntilCompletedFenceReachesTarget) {
    DeferredRenderDeleteQueue queue;
    int destroyed = 0;

    queue.Push(5, make_unique<FakeRenderResource>(&destroyed));
    EXPECT_EQ(queue.Count(), 1u);
    EXPECT_EQ(destroyed, 0);

    queue.Process(4);
    EXPECT_EQ(queue.Count(), 1u);
    EXPECT_EQ(destroyed, 0);

    queue.Process(5);
    EXPECT_EQ(queue.Count(), 0u);
    EXPECT_EQ(destroyed, 1);
}

TEST(DeferredRenderDeleteQueueTest, FlushAlwaysDestroysPendingObjects) {
    DeferredRenderDeleteQueue queue;
    int destroyed = 0;

    queue.Push(10, make_unique<FakeRenderResource>(&destroyed));
    queue.Push(11, make_unique<FakeRenderResource>(&destroyed));
    EXPECT_EQ(queue.Count(), 2u);

    queue.Flush();
    EXPECT_EQ(queue.Count(), 0u);
    EXPECT_EQ(destroyed, 2);
}
