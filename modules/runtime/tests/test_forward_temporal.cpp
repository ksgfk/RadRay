#include "runtime_test_support.h"
#include "foundation_graph_fixture.h"
#include "upload_test_support.h"
#include "forward_pipeline/forward_bindings.h"
#include "forward_pipeline/forward_frame.h"
#include "forward_pipeline/forward_lit_mesh_pass_processor.h"

#include <cstring>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray {
namespace {

// The production uploader writes these buffers and the production set builder supplies the
// binding handle/range. Decoding below follows the resulting set plus its dynamic offset;
// it never reads the processor's source row or PrimitiveHistory::Lookup as its oracle.
class TemporalCaptureSet final : public render::ShaderParameterSet {
public:
    explicit TemporalCaptureSet(uint32_t group) : Group(group) {}
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool Set(render::BindingHandle binding, uint32_t element, render::ShaderParameterValue value) noexcept override {
        const auto* buffer = std::get_if<render::ShaderBufferBinding>(&value);
        if (!binding.IsValid() || element != 0 || !buffer || !buffer->Target || Buffer.has_value()) return false;
        Binding = binding;
        Buffer = *buffer;
        return true;
    }
    bool FlushWrites() noexcept override { Flushed = Buffer.has_value(); return Flushed; }
    uint32_t Group;
    render::BindingHandle Binding;
    std::optional<render::ShaderBufferBinding> Buffer;
    bool Flushed{false};
};

class TemporalCaptureDevice final : public test::GraphCompileDevice {
public:
    explicit TemporalCaptureDevice(render::Device& native) : Native(native) {}
    render::RenderBackend GetBackend() noexcept override { return Native.GetBackend(); }
    const render::RenderDeviceCapabilities& GetCapabilities() const noexcept override { return Native.GetCapabilities(); }
    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor& desc) noexcept override { return Native.CreateShader(desc); }
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& desc) noexcept override {
        return unique_ptr<render::Buffer>{new test::UploadTestBuffer(this, desc, LiveDeviceBuffers)};
    }
    Nullable<unique_ptr<render::ShaderParameterSet>> CreateShaderParameterSet(const render::ShaderParameterSetDescriptor& desc) noexcept override {
        return unique_ptr<render::ShaderParameterSet>{new TemporalCaptureSet(desc.GroupIndex)};
    }

private:
    render::Device& Native;
    int LiveDeviceBuffers{0};
};

template <class T>
std::optional<T> DecodeGroup(const RendererList& list, uint32_t groupIndex, render::BindingHandle binding) {
    if (list.GetDrawCount() != 1) return std::nullopt;
    const auto groups = list.GetGroups(0);
    for (size_t index = 0; index < groups.size(); ++index) {
        const auto& group = groups[index];
        if (group.Group != groupIndex) continue;
        if (!group.Set || group.DynamicOffsets.size() != 1 || group.DynamicOffsets[0].Binding != binding) return std::nullopt;
        const auto* set = static_cast<const TemporalCaptureSet*>(group.Set.Get());
        if (!set->Flushed || set->Group != groupIndex || set->Binding != binding || !set->Buffer) return std::nullopt;
        const auto& buffer = *set->Buffer;
        const uint64_t offset = buffer.Range.Offset + group.DynamicOffsets[0].Offset;
        const auto size = buffer.Target->GetDesc().Size;
        if (buffer.Range.Size != sizeof(T) || offset > size || sizeof(T) > size - offset) return std::nullopt;
        auto* bytes = buffer.Target->Map(offset, sizeof(T));
        if (!bytes) return std::nullopt;
        T value;
        std::memcpy(&value, bytes, sizeof(T));
        buffer.Target->Unmap();
        return value;
    }
    return std::nullopt;
}

class TemporalPrimitive final : public PrimitiveSceneProxy {
public:
    explicit TemporalPrimitive(Material* material) : DrawMaterial(material) {}
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return 1; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {&Geometry, 0, 3, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
    Material* DrawMaterial;
    GpuMesh::DrawData Geometry;
};

struct TemporalReference {
    Eigen::Matrix4f Object{Eigen::Matrix4f::Identity()}, View{Eigen::Matrix4f::Identity()};
    uint64_t Serial{0};
    uint32_t Commits{0};
    bool Valid{false};
};
struct ForwardTemporalResult {
    array<TemporalReference, 2> Views;
    uint32_t Frames{0}, DecodedObjects{0}, CleanFrames{0}, RejectedPreparations{0}, SkippedViews{0};
    uint32_t DivergentCleanFrames{0}, CleanHistoryAdvances{0}, MaterialEditsWithValidMotion{0};
    array<bool, 2> FlightsSeen{};
};

Eigen::Matrix4f ObjectTransform(bool moved) {
    Eigen::Matrix4f result = Eigen::Matrix4f::Identity();
    result(0, 0) = moved ? 2.0f : 1.0f;
    result(1, 1) = 1.5f;
    result(0, 1) = moved ? .25f : -.25f;
    result(0, 3) = moved ? 7.0f : 1.0f;
    result(1, 3) = moved ? -3.0f : 2.0f;
    return result;
}

class ForwardTemporalPipeline final : public RenderPipeline {
public:
    explicit ForwardTemporalPipeline(ForwardTemporalResult& result) : Result(result) {}
    void Initialize(render::Device& native) {
        Device = make_unique<TemporalCaptureDevice>(native);
        auto program = test::CompileFoundationGraphics(native, R"hlsl(
#include <core/platform.hlsli>
#include <pipelines/forward/cbuffers.hlsli>
VK_BINDING(0, 2) ConstantBuffer<Forward_ViewData> ForwardView : register(b0, space4);
VK_BINDING(0, 5) ConstantBuffer<Forward_MaterialData> ForwardMaterial : register(b0, space7);
VK_BINDING(0, 8) ConstantBuffer<Forward_ObjectData> ForwardObject : register(b0, space9);
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position {
    float4 current = mul(ForwardView.ViewProj, mul(ForwardObject.LocalToWorld, float4(p, 1)));
    float4 previous = mul(ForwardView.PreviousViewProj, mul(ForwardObject.PreviousLocalToWorld, float4(p, 1)));
    return current + previous * ForwardObject.MotionValid * .0001;
}
[shader("pixel")] float4 PSMain() : SV_Target0 { return ForwardMaterial.BaseColor; }
)hlsl", ForwardPipeline::GetLayoutRecipe(), Device.get());
        ASSERT_TRUE(program);
        Program = program.Release();
        const auto bindings = forward_detail::ResolveProgramBindings(*Program);
        ASSERT_TRUE(bindings);
        Bindings = *bindings;
        auto technique = MaterialTechnique::Create({{"ForwardLit", Program.get(), "ForwardMaterial", {}}}, "ForwardLit");
        ASSERT_TRUE(technique);
        Technique = technique.Release();
        auto material = Material::Create(Technique.get());
        ASSERT_TRUE(material);
        Authoring = material.Release();
        ASSERT_TRUE(Authoring->SetFloat4("BaseColor", Eigen::Vector4f{1, 0, 0, 1}));
        auto vertex = Device->CreateBuffer({.Size = 36, .Memory = render::MemoryType::Upload, .Usage = render::BufferUse::Vertex});
        auto index = Device->CreateBuffer({.Size = 12, .Memory = render::MemoryType::Upload, .Usage = render::BufferUse::Index});
        ASSERT_TRUE(vertex);
        ASSERT_TRUE(index);
        Vertex = vertex.Release();
        Index = index.Release();
        auto primitive = make_unique<TemporalPrimitive>(Authoring.get());
        primitive->Geometry.VertexBuffers = {{0, {Vertex.get(), 0, 36}}};
        primitive->Geometry.Ibv = {Index.get(), 0, 4};
        primitive->Geometry.VertexLayout.Buffers = {{0, 12, render::VertexStepMode::Vertex}};
        primitive->Geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}};
        primitive->SetLocalToWorld(ObjectTransform(false));
        Primitive = primitive.get();
        RenderScene.AddPrimitive(std::move(primitive));
        for (auto& flight : Flights) flight.Resources = make_unique<FrameDrawResources>(Device.get());
        Initialized = true;
    }
    void CollectScenePolicies(RenderPrepareContext& context) override {
        ASSERT_TRUE(Initialized);
        ++Frame;
        if (Frame == 2) Primitive->SetLocalToWorld(ObjectTransform(true));
        if (Frame == 5 || Frame == 9) {
            ASSERT_TRUE(Authoring->SetFloat4("BaseColor", Frame == 5 ? Eigen::Vector4f{0, 1, 0, 1} : Eigen::Vector4f{0, 0, 1, 1}));
        }
        ASSERT_TRUE(forward_detail::RegisterForwardPassPolicies(context, RenderScene));
    }
    void PrepareFrame(RenderPrepareContext& context) override {
        ASSERT_TRUE(Initialized);
        ASSERT_LT(context.App.FlightIndex, Flights.size());
        auto& flight = Flights[context.App.FlightIndex];
        auto snapshot = context.PrepareScene(RenderScene);
        ASSERT_TRUE(snapshot);
        flight.Scene = snapshot.Release();
        ASSERT_TRUE(flight.Scene->Valid);
        ASSERT_EQ(flight.Scene->Primitives.size(), 1u);
        ASSERT_EQ(flight.Scene->Materials.size(), 1u);
        EXPECT_TRUE(flight.Scene->HasPassPolicies);
        flight.Updates = flight.Objects.Update(*flight.Scene);
        EXPECT_EQ(flight.Objects.Update(*flight.Scene), 0u);
        Result.FlightsSeen[context.App.FlightIndex] = true;
        if (Frame >= 4) {
            EXPECT_EQ(flight.Updates, 0u) << "clean frame " << Frame;
            ++Result.CleanFrames;
        }
        if (Frame == 5 || Frame == 9) {
            EXPECT_GT(flight.Scene->Stats.MaterialBytesCopied, 0u);
            EXPECT_EQ(flight.Scene->Stats.PrimitiveBoundsRebuilt, 0u);
        }
        for (const auto& output : context.Outputs) {
            if (!output.Active || output.Kind != RenderOutputKind::Presentation) continue;
            RenderViewFamilyDesc family;
            family.Output = output.Id;
            for (uint32_t viewIndex = 0; viewIndex < 2; ++viewIndex) {
                RenderViewDesc view;
                view.StateId = Ids[viewIndex];
                view.Name = viewIndex == 0 ? "A" : "B";
                view.WorldToView(0, 3) = static_cast<float>(Frame * 2 + viewIndex);
                view.ViewRect = {viewIndex * .5f, 0, .5f, 1};
                view.ScissorRect = view.ViewRect;
                view.CameraCut = Frame == 6 && viewIndex == 0;
                family.Views.push_back(view);
            }
            context.Workloads.AddViewFamily(std::move(family));
        }
    }
    void BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
        ASSERT_EQ(context.ViewFamilies().size(), 1u);
        const auto& family = context.ViewFamilies().front();
        ASSERT_EQ(family.Views.size(), 2u);
        ASSERT_TRUE(family.OutputAvailable);
        auto outputBinding = FindGraphOutput(outputs, family.OutputId);
        ASSERT_TRUE(outputBinding);
        auto& output = outputBinding->Texture;
        auto& flight = Flights[context.FlightIndex()];
        ASSERT_TRUE(flight.Scene);
        const auto& snapshot = *flight.Scene;
        flight.Writes.Reset();
        ASSERT_TRUE(flight.Resources->BeginFrame(flight.Writes));
        bool overflow = false;
        forward_detail::ForwardLitMeshPassProcessor processor{*flight.Resources, BindingCache, overflow, flight.Objects.Rows(), &context};
        Tokens = {};
        Prepared = {};
        array<RendererList, 2> lists;
        for (uint32_t viewIndex = 0; viewIndex < 2; ++viewIndex) {
            SCOPED_TRACE(fmt::format("frame {} flight {} view {}", Frame, context.FlightIndex(), viewIndex));
            auto view = family.Views[viewIndex];
            auto& reference = Result.Views[viewIndex];
            if (view.CameraCut) reference.Valid = false;
            EXPECT_EQ(context.GetPrimitiveHistoryStamp(view.StateId).CommittedSerial, reference.Serial);
            ASSERT_TRUE(context.PreparePrimitiveHistory(view, snapshot));
            EXPECT_EQ(view.PreviousViewValid, reference.Valid);
            CullingResults culling;
            culling.Scene = &snapshot;
            culling.View = &view;
            culling.Stats.Valid = true;
            culling.Primitives.push_back({0, 0});
            RendererListDesc desc{"temporal object", "ForwardLit", &culling, &view};
            desc.Policy = forward_detail::kForwardLitPolicy;
            desc.RequireMaterialPass = true;
            processor.ResetView();
            const bool failPreparation = (Frame == 3 && viewIndex == 1) || (Frame == 6 && viewIndex == 0);
            if (failPreparation) flight.Resources->FailNextGroupForTesting();
            ASSERT_TRUE(BuildRendererList(desc, processor, lists[viewIndex]));
            Prepared[viewIndex] = lists[viewIndex].Stats.ContentSucceeded();
            EXPECT_EQ(Prepared[viewIndex], !failPreparation);
            if (failPreparation) {
                EXPECT_EQ(lists[viewIndex].GetDrawCount(), 0u);
                EXPECT_EQ(lists[viewIndex].Stats.PrepareResourceFailed, 1u);
                ++Result.RejectedPreparations;
            } else {
                CheckBindings(lists[viewIndex], view, snapshot, reference);
                ++Result.DecodedObjects;
            }
            PendingViews[viewIndex] = view;
            PendingObject = snapshot.Primitives[0].LocalToWorld;
            const bool skip = Frame == 2 && viewIndex == 1;
            RgPassHandle pass;
            if (skip) {
                pass = graph.AddComputePass<test::EmptyGraphPass>("skipped view B", [](test::EmptyGraphPass&, RenderGraphComputeBuilder&) {}, +[](const test::EmptyGraphPass&, RenderGraphComputeContext&) {});
                ++Result.SkippedViews;
            } else {
                output = graph.NextVersion(output);
                pass = graph.AddRasterPass<test::EmptyGraphPass>(fmt::format("view {} output proof", viewIndex), [=](test::EmptyGraphPass&, RenderGraphRasterBuilder& builder) {
                    builder.SetColorAttachment(0, output, {.Load = viewIndex == 0 ? render::LoadAction::Clear : render::LoadAction::Load});
                }, +[](const test::EmptyGraphPass&, RenderGraphRasterContext&) {});
            }
            Tokens[viewIndex] = context.RegisterViewCompletion(graph, view.StateId, pass, output);
            ASSERT_TRUE(Tokens[viewIndex].IsValid());
        }
        // The second view may share a descriptor set, but must not overwrite the first view's bytes.
        for (uint32_t viewIndex = 0; viewIndex < 2; ++viewIndex)
            if (Prepared[viewIndex]) CheckBindings(lists[viewIndex], PendingViews[viewIndex], snapshot, Result.Views[viewIndex]);
        if (Frame == 4) {
            EXPECT_EQ(flight.Updates, 0u);
            EXPECT_FALSE(Result.Views[0].Object.isApprox(Result.Views[1].Object));
            ++Result.DivergentCleanFrames;
        }
        if (Frame == 5) {
            EXPECT_EQ(flight.Updates, 0u);
            EXPECT_TRUE(Result.Views[1].Object.isApprox(ObjectTransform(true)));
            ++Result.CleanHistoryAdvances;
        }
        if (Frame == 5 || Frame == 9) {
            EXPECT_TRUE(Result.Views[0].Valid && Result.Views[1].Valid);
            ++Result.MaterialEditsWithValidMotion;
        }
        flight.Writes.Flush(*Device);
    }
    void GraphRecorded(RenderPipelineContext& context, const RenderGraph&, RenderGraphExecutionResult execution) override {
        ASSERT_TRUE(execution.Success);
        ASSERT_TRUE(execution.Submission);
        array<bool, 2> accepted{};
        for (uint32_t viewIndex = 0; viewIndex < 2; ++viewIndex) {
            accepted[viewIndex] = context.CommitView(Ids[viewIndex], Tokens[viewIndex], Prepared[viewIndex]);
            EXPECT_EQ(accepted[viewIndex], Prepared[viewIndex] && !(Frame == 2 && viewIndex == 1));
            // Queuing the commit must not advance either the registry or our reference.
            EXPECT_EQ(context.GetPrimitiveHistoryStamp(Ids[viewIndex]).CommittedSerial, Result.Views[viewIndex].Serial);
        }
        const auto prior = execution.Submission->OnSubmitted;
        auto* result = &Result;
        const auto views = PendingViews;
        const Eigen::Matrix4f object = PendingObject;
        const auto serial = context.FrameSerial();
        execution.Submission->OnSubmitted = [prior, result, accepted, views, object, serial] {
            if (prior) prior();
            for (uint32_t viewIndex = 0; viewIndex < 2; ++viewIndex) {
                if (!accepted[viewIndex]) continue;
                auto& reference = result->Views[viewIndex];
                reference.Object = object;
                reference.View = views[viewIndex].ViewProjection;
                reference.Serial = serial;
                reference.Valid = true;
                ++reference.Commits;
            }
            ++result->Frames;
        };
    }

private:
    void CheckBindings(const RendererList& list, const ResolvedRenderView& view, const RenderSceneSnapshot& snapshot, const TemporalReference& reference) {
        ASSERT_EQ(list.GetDrawCount(), 1u);
        EXPECT_TRUE(list.Commands.empty());
        ASSERT_TRUE(list.GetBindingId(0).IsValid());
        const auto buffers = Program->GetParameterLayout().Buffers();
        const auto object = DecodeGroup<Forward_ObjectData>(list, Bindings.ObjectGroup, buffers[Bindings.ObjectBufferIndex].Binding);
        ASSERT_TRUE(object);
        EXPECT_EQ(object->MotionValid, reference.Valid ? 1u : 0u);
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(object->LocalToWorld).isApprox(snapshot.Primitives[0].LocalToWorld));
        const Eigen::Matrix4f expected = reference.Valid ? reference.Object : snapshot.Primitives[0].LocalToWorld;
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(object->PreviousLocalToWorld).isApprox(expected));
        const auto viewData = DecodeGroup<Forward_ViewData>(list, Bindings.ViewGroup, buffers[Bindings.ViewBufferIndex].Binding);
        ASSERT_TRUE(viewData);
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(viewData->ViewProj).isApprox(view.ViewProjection));
        const Eigen::Matrix4f previousView = reference.Valid ? reference.View : view.ViewProjection;
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(viewData->PreviousViewProj).isApprox(previousView));
        const auto material = DecodeGroup<Forward_MaterialData>(list, Bindings.MaterialGroup, buffers[Bindings.MaterialBufferIndex].Binding);
        ASSERT_TRUE(material);
        const Eigen::Vector4f expectedColor = Frame < 5 ? Eigen::Vector4f{1, 0, 0, 1} : Frame < 9 ? Eigen::Vector4f{0, 1, 0, 1} : Eigen::Vector4f{0, 0, 1, 1};
        EXPECT_TRUE(static_cast<Eigen::Vector4f>(material->BaseColor).isApprox(expectedColor));
    }
    struct Flight {
        HostWriteBatch Writes;
        unique_ptr<FrameDrawResources> Resources;
        shared_ptr<const RenderSceneSnapshot> Scene;
        forward_detail::ForwardObjectDataCache Objects;
        uint64_t Updates{0};
    };
    ForwardTemporalResult& Result;
    unique_ptr<TemporalCaptureDevice> Device;
    unique_ptr<ShaderProgram> Program;
    unique_ptr<MaterialTechnique> Technique;
    unique_ptr<Material> Authoring;
    unique_ptr<render::Buffer> Vertex, Index;
    Scene RenderScene;
    Nullable<TemporalPrimitive*> Primitive{nullptr};
    array<Flight, 2> Flights;
    forward_detail::ForwardBindingCache BindingCache;
    forward_detail::ForwardProgramBindings Bindings{};
    array<ViewStateId, 2> Ids{AllocateViewStateId(), AllocateViewStateId()};
    array<ViewCompletionToken, 2> Tokens;
    array<ResolvedRenderView, 2> PendingViews;
    array<bool, 2> Prepared{};
    Eigen::Matrix4f PendingObject{Eigen::Matrix4f::Identity()};
    uint32_t Frame{0};
    bool Initialized{false};
};

class ForwardTemporalHost final : public Application {
public:
    explicit ForwardTemporalHost(ForwardTemporalResult& result) : Result(result) {}
    void OnInit() override {
        SetRenderGraphRuntimeOptions(kDiagnosticRenderGraphRuntimeOptions);
        auto pipeline = make_unique<ForwardTemporalPipeline>(Result);
        pipeline->Initialize(*GetDevice());
        GetRenderSystem()->SetPipeline(std::move(pipeline));
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (Result.Frames >= 12 || ++Updates >= 16) test::CloseMainWindow(*this);
    }

private:
    ForwardTemporalResult& Result;
    uint32_t Updates{0};
};

class ForwardTemporalTest : public testing::TestWithParam<render::RenderBackend> {};
TEST_P(ForwardTemporalTest, T51PreparedObjectBindingsTrackIndependentSubmittedViewsAcrossCleanFrames) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(GetParam(), device, true)) GTEST_SKIP() << device.Reason;
    }
    ForwardTemporalResult result;
    test::RuntimeLogCapture logs;
    ForwardTemporalHost app{result};
    ASSERT_EQ(app.Run({.Backend = GetParam(), .EnableValidation = true, .Multithreaded = false,
                       .WindowTitle = "Forward prepared temporal bindings", .WindowWidth = 96, .WindowHeight = 64,
                       .FlightDataCount = 2, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
                       .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_GE(result.Frames, 12u);
    EXPECT_GE(result.DecodedObjects, 22u);
    EXPECT_GE(result.CleanFrames, 9u);
    EXPECT_EQ(result.RejectedPreparations, 2u);
    EXPECT_EQ(result.SkippedViews, 1u);
    EXPECT_EQ(result.DivergentCleanFrames, 1u);
    EXPECT_EQ(result.CleanHistoryAdvances, 1u);
    EXPECT_EQ(result.MaterialEditsWithValidMotion, 2u);
    EXPECT_TRUE(result.FlightsSeen[0] && result.FlightsSeen[1]);
    EXPECT_EQ(result.Views[0].Commits + 1, result.Frames);
    EXPECT_EQ(result.Views[1].Commits + 2, result.Frames);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    RecordProperty("object_data_evidence", "production Forward static preparation, captured RHI buffer binding plus dynamic offset; CPU decoded, not GPU pixels");
    RecordProperty("history_commit_evidence", "real output passes and FrameSubmission OnSubmitted");
}

INSTANTIATE_TEST_SUITE_P(Backends, ForwardTemporalTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

}  // namespace
}  // namespace radray
