#include <gtest/gtest.h>

#include <radray/runtime/render/render_pipeline_executor.h>
#include <radray/runtime/render/render_pipeline.h>
#include <radray/runtime/render/draw_objects_pass.h>
#include <radray/runtime/render/render_context.h>

using namespace radray;
using namespace radray::srp;

namespace {

/// 记录执行顺序的假 pass。覆写 Execute 不触碰 GPU,只把自己的标签塞进共享 log。
class RecordingPass final : public RenderPass {
public:
    RecordingPass(RenderPassEvent ev, int tag, vector<int>* log) : _event(ev), _tag(tag), _log(log) {}

    RenderPassEvent Event() const override { return _event; }
    const WantedLightModes& ShaderTags() const override {
        static WantedLightModes kEmpty{};
        return kEmpty;
    }
    FilteringSettings Filtering() const override { return {}; }
    MeshPassRenderState RenderState(const Material&) const override { return MeshPassRenderState::Opaque(); }
    GpuRenderTargetFormats RTFormats() const override { return {}; }
    render::DescriptorSet* ViewSet(const SceneView&) const override { return nullptr; }
    std::string_view PerObjectParamName() const override { return {}; }
    uint32_t PerObjectByteSize() const override { return 0; }
    void WritePerObject(std::span<byte>, const Renderer&, const SceneView&) const override {}

    // 覆写录制入口:不建 RendererList,只记录顺序。
    void Execute(RenderContext&, const SceneView&, const CullingResults&) override {
        _log->push_back(_tag);
    }

private:
    RenderPassEvent _event;
    int _tag;
    vector<int>* _log;
};

}  // namespace

TEST(SrpExecutorTest, SortStableOrdersByEventKeepsInsertionOrder) {
    // 入队顺序故意打乱,且含两个同 event 的 pass(tag 2 先于 tag 3)。
    vector<int> log;
    RecordingPass transparent{RenderPassEvent::BeforeRenderingTransparents, 1, &log};
    RecordingPass opaqueA{RenderPassEvent::BeforeRenderingOpaques, 2, &log};
    RecordingPass opaqueB{RenderPassEvent::BeforeRenderingOpaques, 3, &log};
    RecordingPass shadow{RenderPassEvent::BeforeRenderingShadows, 4, &log};

    RenderPipelineExecutor executor;
    executor.EnqueuePass(&transparent);
    executor.EnqueuePass(&opaqueA);
    executor.EnqueuePass(&opaqueB);
    executor.EnqueuePass(&shadow);
    EXPECT_EQ(executor.Count(), 4u);

    RenderContext ctx{nullptr, nullptr};
    SceneView view{};
    CullingResults cull{};
    executor.Execute(ctx, view, cull);

    // 期望:shadow(50) → opaqueA(250) → opaqueB(250, 稳定保持入队顺序) → transparent(450)。
    ASSERT_EQ(log.size(), 4u);
    EXPECT_EQ(log[0], 4);
    EXPECT_EQ(log[1], 2);
    EXPECT_EQ(log[2], 3);
    EXPECT_EQ(log[3], 1);

    // Execute 后队列清空。
    EXPECT_TRUE(executor.Empty());
}

TEST(SrpExecutorTest, EnqueueNullIgnored) {
    RenderPipelineExecutor executor;
    executor.EnqueuePass(nullptr);
    EXPECT_TRUE(executor.Empty());
}

TEST(SrpPipelineTest, RenderSingleCameraDrivesSetupThenExecute) {
    vector<int> log;
    RecordingPass a{RenderPassEvent::BeforeRenderingOpaques, 10, &log};
    RecordingPass b{RenderPassEvent::BeforeRenderingShadows, 11, &log};

    RenderPipeline pipeline{[&](RenderPipelineExecutor& exec, const SceneView&, const CullingResults&) {
        exec.EnqueuePass(&a);
        exec.EnqueuePass(&b);
    }};

    RenderContext ctx{nullptr, nullptr};
    CullingResults cull{};
    pipeline.RenderSingleCamera(ctx, cull);

    // setup 入队 a(250)、b(50);排序后 b 先于 a。
    ASSERT_EQ(log.size(), 2u);
    EXPECT_EQ(log[0], 11);
    EXPECT_EQ(log[1], 10);
}

TEST(SrpPipelineTest, RenderMultipleCamerasReRunsSetupPerCamera) {
    vector<int> log;
    RecordingPass p{RenderPassEvent::BeforeRenderingOpaques, 7, &log};

    int setupCalls = 0;
    RenderPipeline pipeline{[&](RenderPipelineExecutor& exec, const SceneView&, const CullingResults&) {
        ++setupCalls;
        exec.EnqueuePass(&p);
    }};

    RenderContext ctx{nullptr, nullptr};
    CullingResults culls[2]{};
    pipeline.Render(ctx, std::span<const CullingResults>{culls, 2});

    EXPECT_EQ(setupCalls, 2);
    EXPECT_EQ(log.size(), 2u);
}

TEST(SrpDrawObjectsPassTest, DescConfigurationFlowsThrough) {
    DrawObjectsPass::Desc desc{};
    desc.Event = RenderPassEvent::BeforeRenderingTransparents;
    desc.ShaderTags = {"UniversalForward", "SRPDefaultUnlit"};
    desc.SortFlags = SortingCriteria::BackToFront;
    desc.PerObjectParamName = "ObjectCB";
    desc.PerObjectByteSize = 64;
    int writes = 0;
    desc.PerObjectFn = [&](std::span<byte>, const Renderer&, const SceneView&) { ++writes; };

    DrawObjectsPass pass{std::move(desc)};
    EXPECT_EQ(pass.Event(), RenderPassEvent::BeforeRenderingTransparents);
    EXPECT_EQ(pass.SortFlags(), SortingCriteria::BackToFront);
    ASSERT_EQ(pass.ShaderTags().size(), 2u);
    EXPECT_EQ(pass.ShaderTags()[0], "UniversalForward");
    EXPECT_EQ(pass.PerObjectParamName(), "ObjectCB");
    EXPECT_EQ(pass.PerObjectByteSize(), 64u);
    EXPECT_EQ(pass.ViewSet(SceneView{}), nullptr);  // 未配 ViewSetFn → nullptr
}
