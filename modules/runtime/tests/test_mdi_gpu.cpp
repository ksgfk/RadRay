#include "../../gpu_impl/tests/gpu_test_fixture.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/gpu_pipeline_layout.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/material.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/mesh_pass_processor.h>
#include <radray/runtime/pipeline_layout_registry.h>
#include <radray/runtime/render_graph_pipeline.h>
#include <radray/runtime/scene.h>
#include <radray/runtime/shader_asset.h>
#include <radray/shader/shader_compiler_contract.h>

namespace radray {
namespace {

using namespace gpu;

struct MdiVertex {
    float Pos[2];
    float Color[4];
};

struct MdiInstance {
    float Offset[4];
    float Tint[4];
};

struct MdiQuadResources {
    unique_ptr<PipelineLayoutRegistry> Registry;
    shared_ptr<GpuPipelineLayout> PipelineLayout;
    unique_ptr<GraphicsPipelineState> Pipeline;
    VertexInput GeometryInput;
    unique_ptr<Buffer> VertexBuffer;
    unique_ptr<Buffer> IndexBuffer;
    unique_ptr<Buffer> InstanceBuffer;
    unique_ptr<Texture> ColorTarget;
};

array<MdiVertex, 6> MakeQuadVertices(float x0, float y0, float x1, float y1, array<float, 4> color) noexcept {
    const MdiVertex v0{{x0, y0}, {color[0], color[1], color[2], color[3]}};
    const MdiVertex v1{{x1, y0}, {color[0], color[1], color[2], color[3]}};
    const MdiVertex v2{{x1, y1}, {color[0], color[1], color[2], color[3]}};
    const MdiVertex v3{{x0, y1}, {color[0], color[1], color[2], color[3]}};
    return {v0, v1, v2, v0, v2, v3};
}

bool InitMdiResources(Device& device, MdiQuadResources& out) {
    constexpr std::string_view kSource = R"(
#include <core/platform.hlsli>
struct VSIn {
    float2 Pos : POSITION;
    float4 Color : COLOR;
    float4 InstOffset : TEXCOORD0;
    float4 InstTint : TEXCOORD1;
};
struct VSOut {
    float4 Position : SV_Position;
    float4 Color : COLOR;
};
[shader("vertex")]
VSOut VSMain(VSIn input) {
    VSOut o;
    o.Position = float4(input.Pos + input.InstOffset.xy, 0.0, 1.0);
    o.Color = input.Color * input.InstTint;
    return o;
}
[shader("pixel")]
float4 PSMain(VSOut input) : SV_Target0 {
    return input.Color;
}
)";
    shader::Client compiler;
    const auto uri = Uri::TryCreate("virtual://tests/mdi_quad.hlsl");
    if (!uri) return false;
    const array<std::filesystem::path, 1> includePaths{std::filesystem::path{RADRAY_SHADERLIB_DIR}};
    const ShaderCompileDesc compileDesc{
        .SourceUri = *uri,
        .Source = std::as_bytes(std::span{kSource}),
        .IncludePaths = includePaths,
        .Variants = {{"Default", {}}}};
    auto compiled = ShaderAsset::Compile(device, compiler, compileDesc);
    EXPECT_TRUE(compiled.Succeeded()) << compiled.Diagnostics;
    if (!compiled.Succeeded()) return false;

    out.Registry = make_unique<PipelineLayoutRegistry>(device);
    auto layout = out.Registry->Acquire(compiled.Value->GetLayoutDesc());
    if (!layout) return false;
    out.PipelineLayout = layout.Release();

    // 顶点缓冲布局：
    // [0..5]:   左上半区红色矩形（用于索引绘制 0）
    // [6..11]:  右上半区绿色矩形（用于索引绘制 1）
    // [12..17]: 左下半区蓝色矩形（用于无索引绘制）
    vector<MdiVertex> vertices;
    const auto q0 = MakeQuadVertices(-1.0f, 0.0f, 0.0f, 1.0f, {1.0f, 0.0f, 0.0f, 1.0f});
    const auto q1 = MakeQuadVertices(0.0f, 0.0f, 1.0f, 1.0f, {0.0f, 1.0f, 0.0f, 1.0f});
    const auto q2 = MakeQuadVertices(-1.0f, -1.0f, 0.0f, 0.0f, {0.0f, 0.0f, 1.0f, 1.0f});
    vertices.insert(vertices.end(), q0.begin(), q0.end());
    vertices.insert(vertices.end(), q1.begin(), q1.end());
    vertices.insert(vertices.end(), q2.begin(), q2.end());

    array<uint32_t, 12> indices{};
    for (uint32_t i = 0; i < 12; ++i) indices[i] = i;

    const array<MdiInstance, 2> instances{
        MdiInstance{{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},
        MdiInstance{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}}};

    out.VertexBuffer = gpu::test::CreateUploadBuffer(device, std::as_bytes(std::span{vertices}), BufferUse::Vertex).Unwrap();
    out.IndexBuffer = gpu::test::CreateUploadBuffer(device, std::as_bytes(std::span{indices}), BufferUse::Index).Unwrap();
    out.InstanceBuffer = gpu::test::CreateUploadBuffer(device, std::as_bytes(std::span{instances}), BufferUse::Vertex).Unwrap();
    {
        auto rec = gpu::test::BeginRecording(device);
        gpu::test::Barrier(*rec.Commands, {
            BarrierBufferDescriptor{out.VertexBuffer.get(), BufferState::HostWrite, BufferState::Vertex},
            BarrierBufferDescriptor{out.IndexBuffer.get(), BufferState::HostWrite, BufferState::Index},
            BarrierBufferDescriptor{out.InstanceBuffer.get(), BufferState::HostWrite, BufferState::Vertex}});
        gpu::test::SubmitAndWait(device, rec);
    }
    out.ColorTarget = device.CreateTexture({
        .Dim = TextureDimension::Dim2D,
        .Width = 4,
        .Height = 4,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();

    out.GeometryInput.Layouts = {
        {0, sizeof(MdiVertex), VertexStepMode::Vertex},
        {1, sizeof(MdiInstance), VertexStepMode::Instance}};
    out.GeometryInput.Attributes = {
        {0, 0, "POSITION", 0, VertexFormat::FLOAT32X2},
        {0, 8, "COLOR", 0, VertexFormat::FLOAT32X4},
        {1, 0, "TEXCOORD", 0, VertexFormat::FLOAT32X4},
        {1, 16, "TEXCOORD", 1, VertexFormat::FLOAT32X4}};
    out.GeometryInput.Streams = {
        {out.VertexBuffer.get(), 0, vertices.size() * sizeof(MdiVertex), 0}};

    ResolvedVertexInputState resolved;
    if (!PrepareMaterialVertexInput(*compiled.Value->GetVariantInterface(0), out.GeometryInput, resolved)) return false;

    auto primitive = PrimitiveState::Default();
    primitive.Cull = CullMode::None;
    primitive.UnclippedDepth = false;
    const auto color = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    auto pipeline = device.CreateGraphicsPipelineState({
        .PipelineLayout = &out.PipelineLayout->GetLayout(),
        .VS = *compiled.Value->GetStage(0, ShaderStage::Vertex),
        .PS = *compiled.Value->GetStage(0, ShaderStage::Pixel),
        .VertexInput = {resolved.Layouts, resolved.Attributes},
        .Primitive = primitive,
        .MultiSample = MultiSampleState::Default(),
        .ColorTargets = std::span{&color, 1}});
    if (!pipeline) return false;
    out.Pipeline = pipeline.Release();
    return true;
}

class MdiPassProcessor final : public MeshPassProcessor {
public:
    explicit MdiPassProcessor(GraphicsPipelineState& pipeline) noexcept : _pipeline(&pipeline) {}
    void AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) override {
        BuildMeshDrawCommands(context, {.Pipeline = _pipeline, .StencilReference = StencilRef, .AllowDeferredInstanceStream = true}, output);
    }
    uint8_t StencilRef{0};

private:
    GraphicsPipelineState* _pipeline;
};

struct IndirectArgsGen {
    FrameSlot* Slot{nullptr};
    MeshPassId Pass{};
    uint64_t MaxBytes{0};
    bool Indexed{true};
};

uint64_t PrepareIndirectArgs(IndirectArgsGen& data, RDGUploadPrepareContext& ctx) noexcept {
    auto& list = ctx.GetPassDrawList(data.Pass);
    list.IndirectDraws.Clear();
    MeshPassIndirectDrawCounts counts{};
    if (!CountMeshPassIndirectDraws(list.VisibleCommands, counts)) return data.MaxBytes + 1;
    return data.Indexed
               ? uint64_t{counts.IndexedArgumentCount} * sizeof(DrawIndexedIndirectArguments)
               : uint64_t{counts.NonIndexedArgumentCount} * sizeof(DrawIndirectArguments);
}

bool WriteIndirectArgs(const IndirectArgsGen& data, RDGUploadWriteContext& write) noexcept {
    auto& list = data.Slot->GetPassDrawList(data.Pass);
    if (list.IndirectDraws.Batches.empty()) {
        if (!BuildMeshPassIndirectDraws(list.VisibleCommands, list.IndirectDraws)) return false;
    }
    const size_t expectedBytes = data.Indexed
                                     ? list.IndirectDraws.IndexedArguments.size() * sizeof(DrawIndexedIndirectArguments)
                                     : list.IndirectDraws.NonIndexedArguments.size() * sizeof(DrawIndirectArguments);
    if (expectedBytes != write.Size || expectedBytes != write.Bytes.size()) return false;
    if (expectedBytes != 0) {
        const void* src = data.Indexed
                              ? static_cast<const void*>(list.IndirectDraws.IndexedArguments.data())
                              : static_cast<const void*>(list.IndirectDraws.NonIndexedArguments.data());
        std::memcpy(write.Bytes.data(), src, expectedBytes);
    }
    return true;
}

class MdiGpuTest : public gpu::test::BackendTest {};

TEST_P(MdiGpuTest, MixedIndexedAndNonIndexedNonZeroOffsetMultiChunkAndEmptyFollowUpFrame) {
    auto& device = GetDevice();
    if (!device.GetCapabilities().Features.IndirectDraw || device.GetCapabilities().Limits.MaxDrawIndirectCount < 1) {
        GTEST_SKIP() << "Backend does not support indirect draw";
    }
    MdiQuadResources res;
    ASSERT_TRUE(InitMdiResources(device, res));

    Scene scene;
    const auto mat = scene.AddMaterial({});
    const auto prim = scene.AddPrimitive();
    MeshBatch indexed0;
    indexed0.Material = mat;
    indexed0.VertexInput = &res.GeometryInput;
    indexed0.Elements.push_back({.IndexBuffer = res.IndexBuffer.get(), .FirstIndex = 0, .IndexCount = 6});
    const auto hIdx0 = scene.AddMeshBatch(prim, indexed0);

    MeshBatch indexed1;
    indexed1.Material = mat;
    indexed1.VertexInput = &res.GeometryInput;
    indexed1.Elements.push_back({.IndexBuffer = res.IndexBuffer.get(), .FirstIndex = 6, .IndexCount = 6});
    const auto hIdx1 = scene.AddMeshBatch(prim, indexed1);

    MeshBatch nonIndexed;
    nonIndexed.Material = mat;
    nonIndexed.VertexInput = &res.GeometryInput;
    nonIndexed.Elements.push_back({.IndexBuffer = nullptr, .FirstIndex = 12, .IndexCount = 6});
    const auto hNonIdx = scene.AddMeshBatch(prim, nonIndexed);
    ASSERT_TRUE(hIdx0.IsValid() && hIdx1.IsValid() && hNonIdx.IsValid());

    MdiPassProcessor processor{*res.Pipeline};
    const MeshPassId passId{0};
    const MeshPassRegistration reg{passId, &processor};
    scene.SyncCachedMeshDrawCommands(std::span{&reg, 1});

    struct RunState {
        bool UseMdi{true};
        bool EmptyFrame{false};
        TextureState TargetInitialState{TextureState::Undefined};
        ColorClearValue ClearColor{0.0f, 0.0f, 0.0f, 1.0f};
        uint64_t ObservedIndexedOffset{0};
        uint32_t ObservedBatchCount{999};
        GpuDeviceCapabilities SubmitCaps{};
    };

    class MixedPipeline final : public RenderGraphFramePipeline {
    public:
        MixedPipeline(DeviceContext& ctx, Scene& scene, MdiQuadResources& res, array<MeshBatchHandle, 3> batches, RunState& state)
            : RenderGraphFramePipeline(ctx, 1, 64 * 1024), _scene(scene), _res(res), _batches(batches), _state(state) {}
        void Wait(FrameSlot& slot) noexcept override { slot.GetQueue().Wait(); }

    private:
        void ExtractFrameData(FrameSlot&) noexcept override {}
        void ExtractMeshPass(MeshPassId pass, FrameSlot&, MeshPassFrameDrawList& drawList) noexcept override {
            if (_state.EmptyFrame) return;
            const array<VisibleMeshBatch, 3> visible{
                VisibleMeshBatch{.Batch = _batches[0], .VisibilitySortKey = 0},
                VisibleMeshBatch{.Batch = _batches[1], .VisibilitySortKey = 1},
                VisibleMeshBatch{.Batch = _batches[2], .VisibilitySortKey = 2}};
            ASSERT_TRUE(_scene.GatherVisibleMeshDrawCommands(pass, visible, drawList));
        }
        void SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept override {
            const auto target = graph.ImportTexture(_res.ColorTarget.get(), _state.TargetInitialState, TextureState::RenderTarget);
            const auto vb = graph.ImportBuffer(_res.VertexBuffer.get(), BufferState::Vertex, BufferState::Vertex);
            const auto ib = graph.ImportBuffer(_res.IndexBuffer.get(), BufferState::Index, BufferState::Index);
            const auto inst = graph.ImportBuffer(_res.InstanceBuffer.get(), BufferState::Vertex, BufferState::Vertex);
            if (!_state.UseMdi) {
                struct Data {
                    MdiQuadResources* Res;
                };
                graph.AddRasterPass("direct mixed", Data{&_res}, +[](const Data& d, RDGRasterPassContext& ctx) noexcept {
                    auto& enc = ctx.GetEncoder();
                    enc.SetViewport({0, 0, 4, 4, 0, 1});
                    enc.SetScissor({0, 0, 4, 4});
                    const MeshPassSubmitArgs args{.InstanceStream = VertexBufferBinding{1, {d.Res->InstanceBuffer.get(), 0, 2 * sizeof(MdiInstance)}}};
                    return SubmitMeshDrawCommands(enc, ctx.GetVisibleCommands({0}), args);
                })
                    .UseMeshPass({0})
                    .WriteColor(0, target, LoadAction::Clear, StoreAction::Store, _state.ClearColor)
                    .ReadBuffer(vb, BufferState::Vertex)
                    .ReadBuffer(ib, BufferState::Index)
                    .ReadBuffer(inst, BufferState::Vertex);
                graph.MarkOutput(target);
                return;
            }
            // 前置分配 16 B 动态缓冲，强制同页内 indexedArgBuffer.Offset > 0。
            const array<byte, 16> prefixBytes{};
            const auto prefixBuf = graph.CreateDynamicBuffer(
                RDGDynamicBufferDescriptor{.MaxSize = prefixBytes.size(), .Usage = BufferUse::CBuffer},
                RDGUploadSource::BorrowedBytes(prefixBytes));
            const uint64_t maxIdxBytes = 4 * sizeof(DrawIndexedIndirectArguments);
            const uint64_t maxNonIdxBytes = 4 * sizeof(DrawIndirectArguments);
            const auto idxArgs = graph.CreateDynamicBuffer(
                RDGDynamicBufferDescriptor{.MaxSize = maxIdxBytes, .Usage = BufferUse::Indirect},
                RDGUploadSource::Generated(maxIdxBytes, IndirectArgsGen{&slot, {0}, maxIdxBytes, true}, &WriteIndirectArgs, &PrepareIndirectArgs));
            const auto nonIdxArgs = graph.CreateDynamicBuffer(
                RDGDynamicBufferDescriptor{.MaxSize = maxNonIdxBytes, .Usage = BufferUse::Indirect},
                RDGUploadSource::Generated(maxNonIdxBytes, IndirectArgsGen{&slot, {0}, maxNonIdxBytes, false}, &WriteIndirectArgs, &PrepareIndirectArgs));

            struct PassData {
                FrameSlot* Slot;
                MdiQuadResources* Res;
                RunState* State;
                RDGBufferHandle IndexedArgs;
                RDGBufferHandle NonIndexedArgs;
            };
            graph.AddRasterPass("mdi mixed", PassData{&slot, &_res, &_state, idxArgs, nonIdxArgs}, +[](const PassData& d, RDGRasterPassContext& ctx) noexcept {
                auto& enc = ctx.GetEncoder();
                enc.SetViewport({0, 0, 4, 4, 0, 1});
                enc.SetScissor({0, 0, 4, 4});
                const auto& drawList = d.Slot->GetPassDrawList({0});
                const auto idxSlice = ctx.ResolveBuffer(d.IndexedArgs);
                const auto nonIdxSlice = ctx.ResolveBuffer(d.NonIndexedArgs);
                d.State->ObservedIndexedOffset = idxSlice.Offset;
                d.State->ObservedBatchCount = static_cast<uint32_t>(drawList.IndirectDraws.Batches.size());
                const MeshPassSubmitArgs args{.InstanceStream = VertexBufferBinding{1, {d.Res->InstanceBuffer.get(), 0, 2 * sizeof(MdiInstance)}}};
                return SubmitMeshPassIndirectDraws(enc, drawList.VisibleCommands, drawList.IndirectDraws, idxSlice, nonIdxSlice, d.State->SubmitCaps, args);
            })
                .UseMeshPass({0})
                .WriteColor(0, target, LoadAction::Clear, StoreAction::Store, _state.ClearColor)
                .ReadBuffer(prefixBuf, BufferState::CBuffer)
                .ReadBuffer(vb, BufferState::Vertex)
                .ReadBuffer(ib, BufferState::Index)
                .ReadBuffer(inst, BufferState::Vertex)
                .ReadBuffer(idxArgs, BufferState::Indirect)
                .ReadBuffer(nonIdxArgs, BufferState::Indirect);
            graph.MarkOutput(target);
        }
        bool SubmitFrame(FrameSlot& slot, CommandBuffer& cmd) noexcept override {
            auto* raw = &cmd;
            return slot.GetQueue().Submit({.CmdBuffers = std::span{&raw, 1}});
        }

        Scene& _scene;
        MdiQuadResources& _res;
        array<MeshBatchHandle, 3> _batches;
        RunState& _state;
    };

    DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &gpu::test::DirectQueue(device)};
    RunState state;
    state.SubmitCaps = device.GetCapabilities();
    // 强制 MaxDrawIndirectCount = 1，使 2 条有索引记录必须切分为 2 个 chunk 验证 consumed * stride 推进。
    state.SubmitCaps.Limits.MaxDrawIndirectCount = 1;

    auto scheduler = FrameScheduler::Create(context, make_unique<MixedPipeline>(context, scene, res, array{hIdx0, hIdx1, hNonIdx}, state), 1).Unwrap();

    // 1. 直接绘制基线像素。
    state.UseMdi = false;
    state.EmptyFrame = false;
    state.TargetInitialState = TextureState::Undefined;
    ASSERT_TRUE(scheduler->RunFrame());
    scheduler->WaitIdle();
    const auto directPixels = gpu::test::FromBytes<uint32_t>(gpu::test::ReadTexture(device, *res.ColorTarget, TextureState::RenderTarget));

    // 2. MDI 混合有索引 + 无索引 + 非零切片偏移 + MaxDrawIndirectCount=1 多分块。
    state.UseMdi = true;
    state.EmptyFrame = false;
    state.TargetInitialState = TextureState::CopySource;
    ASSERT_TRUE(scheduler->RunFrame());
    scheduler->WaitIdle();
    const auto mdiPixels = gpu::test::FromBytes<uint32_t>(gpu::test::ReadTexture(device, *res.ColorTarget, TextureState::RenderTarget));

    EXPECT_GT(state.ObservedIndexedOffset, 0u);
    EXPECT_EQ(state.ObservedBatchCount, 2u);  // 1 个有索引合并批次（2 条） + 1 个无索引批次（1 条）
    EXPECT_EQ(mdiPixels, directPixels);
    EXPECT_EQ(std::count(mdiPixels.begin(), mdiPixels.end(), 0xff0000ffu), 4);
    EXPECT_EQ(std::count(mdiPixels.begin(), mdiPixels.end(), 0xff00ff00u), 4);
    EXPECT_EQ(std::count(mdiPixels.begin(), mdiPixels.end(), 0xffff0000u), 4);
    EXPECT_EQ(std::count(mdiPixels.begin(), mdiPixels.end(), 0xff000000u), 4);

    // 3. 同槽下一帧 0 条可见命令（清屏黄色，ActualSize == 0 跳过 Write）：验证 IndirectDraws 已清空且不重放上帧绘制。
    state.EmptyFrame = true;
    state.TargetInitialState = TextureState::CopySource;
    state.ClearColor = {1.0f, 1.0f, 0.0f, 1.0f};
    ASSERT_TRUE(scheduler->RunFrame());
    scheduler->WaitIdle();
    const auto emptyPixels = gpu::test::FromBytes<uint32_t>(gpu::test::ReadTexture(device, *res.ColorTarget, TextureState::RenderTarget));
    EXPECT_EQ(state.ObservedBatchCount, 0u);
    EXPECT_EQ(emptyPixels, vector<uint32_t>(16, 0xff00ffffu));
}

TEST_P(MdiGpuTest, CompactedInstancePrepareLayoutAndWriteMatchesDirectUnderBothRegistrationOrders) {
    auto& device = GetDevice();
    if (!device.GetCapabilities().Features.IndirectDraw || device.GetCapabilities().Limits.MaxDrawIndirectCount < 1) {
        GTEST_SKIP() << "Backend does not support indirect draw";
    }
    MdiQuadResources res;
    ASSERT_TRUE(InitMdiResources(device, res));

    Scene scene;
    const auto mat = scene.AddMaterial({});
    const auto prim = scene.AddPrimitive();
    MeshBatch indexed0;
    indexed0.Material = mat;
    indexed0.VertexInput = &res.GeometryInput;
    indexed0.Elements.push_back({.IndexBuffer = res.IndexBuffer.get(), .FirstIndex = 0, .IndexCount = 6});
    const auto batchHandle = scene.AddMeshBatch(prim, indexed0);
    ASSERT_TRUE(batchHandle.IsValid());

    MdiPassProcessor processor{*res.Pipeline};
    const MeshPassRegistration reg{{0}, &processor};
    scene.SyncCachedMeshDrawCommands(std::span{&reg, 1});

    struct CompactInstanceGen {
        array<MdiInstance, 8> Candidates{};
        vector<uint32_t> Compacted;
    };

    TextureState targetInitialState = TextureState::Undefined;
    const auto runScenario = [&](bool useMdi, bool registerInstancesFirst) -> vector<uint32_t> {
        class CompactPipeline final : public RenderGraphFramePipeline {
        public:
            CompactPipeline(DeviceContext& ctx, Scene& scene, MdiQuadResources& res, MeshBatchHandle batch, bool useMdi, bool instancesFirst, TextureState initTargetState)
                : RenderGraphFramePipeline(ctx, 1, 64 * 1024),
                  _scene(scene),
                  _res(res),
                  _batch(batch),
                  _useMdi(useMdi),
                  _instancesFirst(instancesFirst),
                  _initTargetState(initTargetState) {}
            void Wait(FrameSlot& slot) noexcept override { slot.GetQueue().Wait(); }

        private:
            void ExtractFrameData(FrameSlot&) noexcept override {}
            void ExtractMeshPass(MeshPassId pass, FrameSlot&, MeshPassFrameDrawList& drawList) noexcept override {
                // 候选实例下标 7，待实例生成器 PrepareLayout 紧凑重映射为 0。
                const VisibleMeshBatch vis{.Batch = _batch, .FirstInstanceOverride = 7u, .InstanceCountOverride = 1u};
                ASSERT_TRUE(_scene.GatherVisibleMeshDrawCommands(pass, std::span{&vis, 1}, drawList));
            }
            void SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept override {
                CompactInstanceGen instGen;
                for (auto& item : instGen.Candidates) item = MdiInstance{{0.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
                // 候选 7：将左半区红矩形右移到右半区并保持红色，若未重映射为 0 则实例缓冲越界/错位。
                instGen.Candidates[7] = MdiInstance{{1.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}};

                const auto createInstances = [&]() {
                    const uint64_t maxInstBytes = 8 * sizeof(MdiInstance);
                    return graph.CreateDynamicBuffer(
                        RDGDynamicBufferDescriptor{.MaxSize = maxInstBytes, .Usage = BufferUse::Vertex},
                        RDGUploadSource::Generated(
                            maxInstBytes,
                            std::move(instGen),
                            +[](const CompactInstanceGen& data, RDGUploadWriteContext& write) noexcept {
                                for (size_t i = 0; i < data.Compacted.size(); ++i) {
                                    std::memcpy(write.Bytes.data() + i * sizeof(MdiInstance), &data.Candidates[data.Compacted[i]], sizeof(MdiInstance));
                                }
                                return true;
                            },
                            +[](CompactInstanceGen& data, RDGUploadPrepareContext& ctx) noexcept -> uint64_t {
                                array<uint32_t, 8> map{};
                                map.fill(std::numeric_limits<uint32_t>::max());
                                for (const auto pass : ctx.GetAliveMeshPasses()) {
                                    for (auto& cmd : ctx.GetPassDrawList(pass).VisibleCommands) {
                                        const uint32_t candidate = cmd.FirstInstance;
                                        if (candidate >= map.size()) return 8 * sizeof(MdiInstance) + 1;
                                        if (map[candidate] == std::numeric_limits<uint32_t>::max()) {
                                            map[candidate] = static_cast<uint32_t>(data.Compacted.size());
                                            data.Compacted.push_back(candidate);
                                        }
                                        cmd.FirstInstance = map[candidate];
                                    }
                                }
                                return uint64_t{data.Compacted.size()} * sizeof(MdiInstance);
                            }));
                };

                RDGBufferHandle instBuf{};
                RDGBufferHandle idxArgs{};
                RDGBufferHandle nonIdxArgs{};
                const uint64_t maxIdxBytes = 4 * sizeof(DrawIndexedIndirectArguments);
                const uint64_t maxNonIdxBytes = 4 * sizeof(DrawIndirectArguments);
                const auto createIndirect = [&]() {
                    idxArgs = graph.CreateDynamicBuffer(
                        RDGDynamicBufferDescriptor{.MaxSize = maxIdxBytes, .Usage = BufferUse::Indirect},
                        RDGUploadSource::Generated(maxIdxBytes, IndirectArgsGen{&slot, {0}, maxIdxBytes, true}, &WriteIndirectArgs, &PrepareIndirectArgs));
                    nonIdxArgs = graph.CreateDynamicBuffer(
                        RDGDynamicBufferDescriptor{.MaxSize = maxNonIdxBytes, .Usage = BufferUse::Indirect},
                        RDGUploadSource::Generated(maxNonIdxBytes, IndirectArgsGen{&slot, {0}, maxNonIdxBytes, false}, &WriteIndirectArgs, &PrepareIndirectArgs));
                };

                if (!_useMdi) {
                    instBuf = createInstances();
                } else if (_instancesFirst) {
                    instBuf = createInstances();
                    createIndirect();
                } else {
                    createIndirect();
                    instBuf = createInstances();
                }

                const auto target = graph.ImportTexture(_res.ColorTarget.get(), _initTargetState, TextureState::RenderTarget);
                const auto vb = graph.ImportBuffer(_res.VertexBuffer.get(), BufferState::Vertex, BufferState::Vertex);
                const auto ib = graph.ImportBuffer(_res.IndexBuffer.get(), BufferState::Index, BufferState::Index);

                struct PassData {
                    FrameSlot* Slot;
                    bool UseMdi;
                    RDGBufferHandle Inst;
                    RDGBufferHandle IdxArgs;
                    RDGBufferHandle NonIdxArgs;
                };
                auto pass = graph.AddRasterPass("compact draw", PassData{&slot, _useMdi, instBuf, idxArgs, nonIdxArgs}, +[](const PassData& d, RDGRasterPassContext& ctx) noexcept {
                    auto& enc = ctx.GetEncoder();
                    enc.SetViewport({0, 0, 4, 4, 0, 1});
                    enc.SetScissor({0, 0, 4, 4});
                    const auto instSlice = ctx.ResolveBuffer(d.Inst);
                    const MeshPassSubmitArgs args{.InstanceStream = VertexBufferBinding{1, {instSlice.Buffer.Get(), instSlice.Offset, instSlice.Size}}};
                    if (!d.UseMdi) {
                        return SubmitMeshDrawCommands(enc, ctx.GetVisibleCommands({0}), args);
                    }
                    const auto& list = d.Slot->GetPassDrawList({0});
                    return SubmitMeshPassIndirectDraws(
                        enc, list.VisibleCommands, list.IndirectDraws,
                        ctx.ResolveBuffer(d.IdxArgs), ctx.ResolveBuffer(d.NonIdxArgs),
                        d.Slot->GetDevice().GetCapabilities(), args);
                });
                pass.UseMeshPass({0})
                    .WriteColor(0, target, LoadAction::Clear, StoreAction::Store, {{0.0f, 0.0f, 0.0f, 1.0f}})
                    .ReadBuffer(vb, BufferState::Vertex)
                    .ReadBuffer(ib, BufferState::Index)
                    .ReadBuffer(instBuf, BufferState::Vertex);
                if (_useMdi) {
                    pass.ReadBuffer(idxArgs, BufferState::Indirect)
                        .ReadBuffer(nonIdxArgs, BufferState::Indirect);
                }
                graph.MarkOutput(target);
            }
            bool SubmitFrame(FrameSlot& slot, CommandBuffer& cmd) noexcept override {
                auto* raw = &cmd;
                return slot.GetQueue().Submit({.CmdBuffers = std::span{&raw, 1}});
            }

            Scene& _scene;
            MdiQuadResources& _res;
            MeshBatchHandle _batch;
            bool _useMdi;
            bool _instancesFirst;
            TextureState _initTargetState;
        };

        DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &gpu::test::DirectQueue(device)};
        auto scheduler = FrameScheduler::Create(context, make_unique<CompactPipeline>(context, scene, res, batchHandle, useMdi, registerInstancesFirst, targetInitialState), 1).Unwrap();
        EXPECT_TRUE(scheduler->RunFrame());
        scheduler->WaitIdle();
        auto pixels = gpu::test::FromBytes<uint32_t>(gpu::test::ReadTexture(device, *res.ColorTarget, TextureState::RenderTarget));
        targetInitialState = TextureState::CopySource;
        return pixels;
    };

    const auto directPixels = runScenario(false, true);
    const auto mdiInstancesFirst = runScenario(true, true);
    const auto mdiIndirectFirst = runScenario(true, false);

    EXPECT_EQ(mdiInstancesFirst, directPixels);
    EXPECT_EQ(mdiIndirectFirst, directPixels);
    EXPECT_EQ(std::count(directPixels.begin(), directPixels.end(), 0xff0000ffu), 4);
    EXPECT_EQ(std::count(directPixels.begin(), directPixels.end(), 0xff000000u), 12);
}

TEST_P(MdiGpuTest, BatchLevelIndirectFirstInstanceFallbackMatchesDirectPixels) {
    auto& device = GetDevice();
    if (!device.GetCapabilities().Features.IndirectDraw || device.GetCapabilities().Limits.MaxDrawIndirectCount < 1) {
        GTEST_SKIP() << "Backend does not support indirect draw";
    }
    MdiQuadResources res;
    ASSERT_TRUE(InitMdiResources(device, res));

    const auto whiteQuad = MakeQuadVertices(-1.0f, 0.0f, 0.0f, 1.0f, {1.0f, 1.0f, 1.0f, 1.0f});
    auto whiteVB = gpu::test::CreateUploadBuffer(device, std::as_bytes(std::span{whiteQuad}), BufferUse::Vertex).Unwrap();
    const array<MdiInstance, 2> instances{
        MdiInstance{{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
        MdiInstance{{1.0f, 0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f, 1.0f}}};
    auto instBuf = gpu::test::CreateUploadBuffer(device, std::as_bytes(std::span{instances}), BufferUse::Vertex).Unwrap();
    {
        auto rec = gpu::test::BeginRecording(device);
        gpu::test::Barrier(*rec.Commands, {
            BarrierBufferDescriptor{whiteVB.get(), BufferState::HostWrite, BufferState::Vertex},
            BarrierBufferDescriptor{instBuf.get(), BufferState::HostWrite, BufferState::Vertex}});
        gpu::test::SubmitAndWait(device, rec);
    }

    VertexInput input = res.GeometryInput;
    input.Streams = {{whiteVB.get(), 0, sizeof(whiteQuad), 0}};

    Scene scene;
    const auto mat0 = scene.AddMaterial({});
    const auto mat1 = scene.AddMaterial({});
    const auto prim = scene.AddPrimitive();
    MeshBatch batch0{.Material = mat0, .VertexInput = &input};
    batch0.Elements.push_back({.IndexBuffer = res.IndexBuffer.get(), .FirstIndex = 0, .IndexCount = 6, .FirstInstance = 0, .InstanceCount = 1});
    MeshBatch batch1{.Material = mat1, .VertexInput = &input};
    batch1.Elements.push_back({.IndexBuffer = res.IndexBuffer.get(), .FirstIndex = 0, .IndexCount = 6, .FirstInstance = 1, .InstanceCount = 1});
    const auto h0 = scene.AddMeshBatch(prim, batch0);
    const auto h1 = scene.AddMeshBatch(prim, batch1);
    ASSERT_TRUE(h0.IsValid() && h1.IsValid());

    class TwoBatchProcessor final : public MeshPassProcessor {
    public:
        TwoBatchProcessor(GraphicsPipelineState& pipeline, MaterialProxyHandle m1) noexcept : _pipeline(&pipeline), _m1(m1) {}
        void AddMeshBatch(const MeshBatchContext& context, MeshPassDrawListContext& output) override {
            const uint8_t stencil = context.Batch.Material == _m1 ? 1u : 0u;
            BuildMeshDrawCommands(context, {.Pipeline = _pipeline, .StencilReference = stencil, .AllowDeferredInstanceStream = true}, output);
        }

    private:
        GraphicsPipelineState* _pipeline;
        MaterialProxyHandle _m1;
    };
    TwoBatchProcessor processor{*res.Pipeline, mat1};
    const MeshPassRegistration reg{{0}, &processor};
    scene.SyncCachedMeshDrawCommands(std::span{&reg, 1});

    TextureState targetInitialState = TextureState::Undefined;
    const auto runMode = [&](bool useMdi) -> vector<uint32_t> {
        class FallbackGpuPipeline final : public RenderGraphFramePipeline {
        public:
            FallbackGpuPipeline(DeviceContext& ctx, Scene& scene, MdiQuadResources& res, Buffer& vb, Buffer& inst, array<MeshBatchHandle, 2> batches, bool useMdi, TextureState initTargetState)
                : RenderGraphFramePipeline(ctx, 1, 64 * 1024),
                  _scene(scene),
                  _res(res),
                  _vb(vb),
                  _inst(inst),
                  _batches(batches),
                  _useMdi(useMdi),
                  _initTargetState(initTargetState) {}
            void Wait(FrameSlot& slot) noexcept override { slot.GetQueue().Wait(); }

        private:
            void ExtractFrameData(FrameSlot&) noexcept override {}
            void ExtractMeshPass(MeshPassId pass, FrameSlot&, MeshPassFrameDrawList& drawList) noexcept override {
                const array<VisibleMeshBatch, 2> visible{
                    VisibleMeshBatch{.Batch = _batches[0], .VisibilitySortKey = 0},
                    VisibleMeshBatch{.Batch = _batches[1], .VisibilitySortKey = 1}};
                ASSERT_TRUE(_scene.GatherVisibleMeshDrawCommands(pass, visible, drawList));
            }
            void SetupGraph(RDGBuilder& graph, FrameSlot& slot) noexcept override {
                const auto target = graph.ImportTexture(_res.ColorTarget.get(), _initTargetState, TextureState::RenderTarget);
                const auto vbH = graph.ImportBuffer(&_vb, BufferState::Vertex, BufferState::Vertex);
                const auto ibH = graph.ImportBuffer(_res.IndexBuffer.get(), BufferState::Index, BufferState::Index);
                const auto instH = graph.ImportBuffer(&_inst, BufferState::Vertex, BufferState::Vertex);
                RDGBufferHandle idxArgs{};
                if (_useMdi) {
                    const uint64_t maxBytes = 4 * sizeof(DrawIndexedIndirectArguments);
                    idxArgs = graph.CreateDynamicBuffer(
                        RDGDynamicBufferDescriptor{.MaxSize = maxBytes, .Usage = BufferUse::Indirect},
                        RDGUploadSource::Generated(maxBytes, IndirectArgsGen{&slot, {0}, maxBytes, true}, &WriteIndirectArgs, &PrepareIndirectArgs));
                }
                struct PassData {
                    FrameSlot* Slot;
                    Buffer* Inst;
                    bool UseMdi;
                    RDGBufferHandle IdxArgs;
                };
                auto pass = graph.AddRasterPass("batch fallback gpu", PassData{&slot, &_inst, _useMdi, idxArgs}, +[](const PassData& d, RDGRasterPassContext& ctx) noexcept {
                    auto& enc = ctx.GetEncoder();
                    enc.SetViewport({0, 0, 4, 4, 0, 1});
                    enc.SetScissor({0, 0, 4, 4});
                    const MeshPassSubmitArgs args{.InstanceStream = VertexBufferBinding{1, {d.Inst, 0, 2 * sizeof(MdiInstance)}}};
                    if (!d.UseMdi) return SubmitMeshDrawCommands(enc, ctx.GetVisibleCommands({0}), args);
                    auto caps = d.Slot->GetDevice().GetCapabilities();
                    caps.Features.IndirectDraw = true;
                    caps.Features.IndirectFirstInstance = false;
                    caps.Limits.MaxDrawIndirectCount = 16;
                    const auto& list = d.Slot->GetPassDrawList({0});
                    EXPECT_EQ(list.IndirectDraws.Batches.size(), 2u);
                    if (list.IndirectDraws.Batches.size() == 2u) {
                        EXPECT_FALSE(list.IndirectDraws.Batches[0].HasNonZeroFirstInstance);
                        EXPECT_TRUE(list.IndirectDraws.Batches[1].HasNonZeroFirstInstance);
                    }
                    return SubmitMeshPassIndirectDraws(enc, list.VisibleCommands, list.IndirectDraws, ctx.ResolveBuffer(d.IdxArgs), {}, caps, args);
                });
                pass.UseMeshPass({0})
                    .WriteColor(0, target, LoadAction::Clear, StoreAction::Store, {{0.0f, 0.0f, 0.0f, 1.0f}})
                    .ReadBuffer(vbH, BufferState::Vertex)
                    .ReadBuffer(ibH, BufferState::Index)
                    .ReadBuffer(instH, BufferState::Vertex);
                if (_useMdi) pass.ReadBuffer(idxArgs, BufferState::Indirect);
                graph.MarkOutput(target);
            }
            bool SubmitFrame(FrameSlot& slot, CommandBuffer& cmd) noexcept override {
                auto* raw = &cmd;
                return slot.GetQueue().Submit({.CmdBuffers = std::span{&raw, 1}});
            }

            Scene& _scene;
            MdiQuadResources& _res;
            Buffer& _vb;
            Buffer& _inst;
            array<MeshBatchHandle, 2> _batches;
            bool _useMdi;
            TextureState _initTargetState;
        };

        DeviceContext context{shared_ptr<Device>{&device, [](Device*) {}}, &gpu::test::DirectQueue(device)};
        auto scheduler = FrameScheduler::Create(context, make_unique<FallbackGpuPipeline>(context, scene, res, *whiteVB, *instBuf, array{h0, h1}, useMdi, targetInitialState), 1).Unwrap();
        EXPECT_TRUE(scheduler->RunFrame());
        scheduler->WaitIdle();
        auto pixels = gpu::test::FromBytes<uint32_t>(gpu::test::ReadTexture(device, *res.ColorTarget, TextureState::RenderTarget));
        targetInitialState = TextureState::CopySource;
        return pixels;
    };

    const auto directPixels = runMode(false);
    const auto mdiFallbackPixels = runMode(true);
    EXPECT_EQ(mdiFallbackPixels, directPixels);
    EXPECT_EQ(std::count(mdiFallbackPixels.begin(), mdiFallbackPixels.end(), 0xff0000ffu), 4);
    EXPECT_EQ(std::count(mdiFallbackPixels.begin(), mdiFallbackPixels.end(), 0xff00ff00u), 4);
    EXPECT_EQ(std::count(mdiFallbackPixels.begin(), mdiFallbackPixels.end(), 0xff000000u), 8);
}

INSTANTIATE_TEST_SUITE_P(Backends, MdiGpuTest, testing::ValuesIn(gpu::test::AllBackends), gpu::test::BackendParamName);

}  // namespace
}  // namespace radray
