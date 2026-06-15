#include <gtest/gtest.h>

#include <radray/runtime/renderer/render_pipeline.h>
#include <radray/runtime/renderer/render_pass.h>
#include <radray/runtime/renderer/render_context.h>
#include <radray/runtime/renderer/render_resource_pool.h>
#include <radray/runtime/renderer/scene_renderer.h>

using namespace radray;

namespace {

// 记录执行顺序的假 pass。不碰 GPU:RenderContext 字段保持默认(null),
// 本测试只验证 RenderPipeline 的组织语义(装填 / 按序驱动 / 跳过 null)。
class RecordingPass : public RenderPass {
public:
    RecordingPass(int id, vector<int>* log) noexcept : _id(id), _log(log) {}

    std::string_view GetName() const noexcept override { return "RecordingPass"; }

    void Execute(RenderContext& ctx) override {
        (void)ctx;
        _log->push_back(_id);
    }

private:
    int _id;
    vector<int>* _log;
};

// 捕获共享资源指针的假 pass:验证 RenderPipeline 把 context 上的共享可见集 / 资源池
// 原样透传给每个 pass(跨 pass 共享的管道)。
class CapturingPass : public RenderPass {
public:
    std::string_view GetName() const noexcept override { return "CapturingPass"; }

    void Execute(RenderContext& ctx) override {
        CapturedVisible = ctx.Visible;
        CapturedResources = ctx.Resources;
    }

    const VisiblePrimitiveList* CapturedVisible{nullptr};
    RenderResourcePool* CapturedResources{nullptr};
};

}  // namespace

// 空管线:不含任何 pass,Render 是 no-op,GetPassCount 为 0。
TEST(RenderPipelineTest, EmptyPipelineRendersNothing) {
    RenderPipeline pipeline;
    EXPECT_EQ(pipeline.GetPassCount(), 0u);

    RenderContext ctx{};
    pipeline.Render(ctx);  // 不应崩溃
    EXPECT_EQ(pipeline.GetPassCount(), 0u);
}

// 按插入顺序逐个 Execute。
TEST(RenderPipelineTest, ExecutesPassesInInsertionOrder) {
    vector<int> log;
    RenderPipeline pipeline;
    pipeline.AddPass(make_unique<RecordingPass>(1, &log));
    pipeline.AddPass(make_unique<RecordingPass>(2, &log));
    pipeline.AddPass(make_unique<RecordingPass>(3, &log));
    EXPECT_EQ(pipeline.GetPassCount(), 3u);

    RenderContext ctx{};
    pipeline.Render(ctx);

    ASSERT_EQ(log.size(), 3u);
    EXPECT_EQ(log[0], 1);
    EXPECT_EQ(log[1], 2);
    EXPECT_EQ(log[2], 3);
}

// 多次 Render:每次都按序重跑全部 pass。
TEST(RenderPipelineTest, RenderIsRepeatable) {
    vector<int> log;
    RenderPipeline pipeline;
    pipeline.AddPass(make_unique<RecordingPass>(7, &log));
    pipeline.AddPass(make_unique<RecordingPass>(8, &log));

    RenderContext ctx{};
    pipeline.Render(ctx);
    pipeline.Render(ctx);

    ASSERT_EQ(log.size(), 4u);
    EXPECT_EQ(log[0], 7);
    EXPECT_EQ(log[1], 8);
    EXPECT_EQ(log[2], 7);
    EXPECT_EQ(log[3], 8);
}

// AddPass(nullptr) 被忽略,不计入数量、不影响执行。
TEST(RenderPipelineTest, NullPassIsIgnored) {
    vector<int> log;
    RenderPipeline pipeline;
    pipeline.AddPass(nullptr);
    pipeline.AddPass(make_unique<RecordingPass>(5, &log));
    pipeline.AddPass(nullptr);
    EXPECT_EQ(pipeline.GetPassCount(), 1u);

    RenderContext ctx{};
    pipeline.Render(ctx);

    ASSERT_EQ(log.size(), 1u);
    EXPECT_EQ(log[0], 5);
}

// 共享资源透传:context 上的 Visible / Resources 指针原样到达 pass。
// 这是"cull 一次、各 pass 共享" + "跨 pass 共享资源池"的管道验证。
TEST(RenderPipelineTest, PassesReceiveSharedContextResources) {
    auto pass = make_unique<CapturingPass>();
    CapturingPass* raw = pass.get();
    RenderPipeline pipeline;
    pipeline.AddPass(std::move(pass));

    VisiblePrimitiveList visible;
    RenderResourcePool pool;
    RenderContext ctx{};
    ctx.Visible = &visible;
    ctx.Resources = &pool;
    pipeline.Render(ctx);

    EXPECT_EQ(raw->CapturedVisible, &visible);
    EXPECT_EQ(raw->CapturedResources, &pool);
}

// 资源池:未 Acquire 时 Find 返回 nullptr(查找逻辑)。
// Acquire/GetView/Transition 需真 device,由 example 运行时路径覆盖,不在纯 CPU 单测中。
TEST(RenderResourcePoolTest, FindReturnsNullWhenEmpty) {
    RenderResourcePool pool;
    EXPECT_EQ(pool.Find("SceneDepth", 0), nullptr);
    EXPECT_EQ(pool.Find("SceneDepth", 1), nullptr);
}

// 资源池:空池 Clear 安全、幂等。
TEST(RenderResourcePoolTest, ClearIsSafeWhenEmpty) {
    RenderResourcePool pool;
    pool.Clear();
    EXPECT_EQ(pool.Find("x", 0), nullptr);
}
