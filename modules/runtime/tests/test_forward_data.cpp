#include "runtime_test_support.h"
#include "stage_b_test_support.h"
#include "gpu_test_fixture.h"
#include "forward_pipeline/forward_bindings.h"
#include "forward_pipeline/forward_frame.h"
#include "forward_pipeline/forward_lit_mesh_pass_processor.h"
#include "forward_pipeline/depth_only_mesh_pass_processor.h"

#include <gtest/gtest.h>
#include <utility>

#include <radray/file.h>
#include <radray/runtime/application.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/shader_jit.h>

namespace radray {
namespace {

const std::filesystem::path kProjectRoot{RADRAY_PROJECT_DIR};

TEST(ForwardNormalTransform, MatchesInverseTransposeWithScaleShearAndReflection) {
    for (const Eigen::Vector3f& scale : {Eigen::Vector3f{2, 1, .5f}, Eigen::Vector3f{-2, 3, 1}, Eigen::Vector3f{2e-8f, 1e-8f, .5e-8f}}) {
        Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
        transform.block<3, 3>(0, 0) = Eigen::AngleAxisf{.7f, Eigen::Vector3f{1, 2, 3}.normalized()}.toRotationMatrix() * scale.asDiagonal();
        transform.col(1) += transform.col(0) * .3f;
        const Eigen::Matrix3f linear = transform.block<3, 3>(0, 0);
        const Eigen::Matrix3f normalMatrix = forward_detail::MakeNormalToWorld(transform).block<3, 3>(0, 0);
        const Eigen::Vector3f normal{1, 1, -1};
        const Eigen::Vector3f tangent{1, 0, 1};
        const Eigen::Vector3f actual = (normalMatrix * normal).normalized();
        EXPECT_TRUE(actual.isApprox((linear.inverse().transpose() * normal).normalized(), 1e-5f));
        EXPECT_NEAR(actual.dot((linear * tangent).normalized()), 0, 1e-5f);
    }
}

TEST(ForwardNormalTransform, SingularTransformsStayFiniteAndKeepSurvivingPlaneNormal) {
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(0, 0) = 2;
    transform(2, 2) = 0;
    const auto normal = forward_detail::MakeNormalToWorld(transform);
    EXPECT_TRUE(normal.allFinite());
    EXPECT_TRUE((normal.block<3, 3>(0, 0) * Eigen::Vector3f{1, 0, -1}).isApprox(Eigen::Vector3f{0, 0, -1}));
    transform(1, 1) = 0;
    EXPECT_TRUE(forward_detail::MakeNormalToWorld(transform).allFinite());
    EXPECT_TRUE(forward_detail::MakeNormalToWorld(Eigen::Matrix4f::Zero()).allFinite());
}

TEST(ForwardObjectValues, T46EveryWarmFlightReusesNormalAndObjectRowsForCameraOnlyFrames) {
    RenderSceneSnapshot scene;
    scene.Primitives.push_back({.Generation = 7, .TransformRevision = 1});
    array<forward_detail::ForwardObjectDataCache, 3> flights;
    for (auto& flight : flights) EXPECT_EQ(flight.Update(scene), 1u);
    for (uint32_t frame = 0; frame < 1000; ++frame) {
        auto& flight = flights[frame % flights.size()];
        EXPECT_EQ(flight.Update(scene), 0u);
        const auto* row = AsCBuffer<Forward_ObjectData>(flight.Rows().Row(0));
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(row->LocalToWorld).isIdentity());
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(row->PreviousLocalToWorld).isIdentity());
        EXPECT_EQ(row->MotionValid, 0u);
    }
    scene.Primitives[0].LocalToWorld(0, 0) = -2;
    scene.Primitives[0].LocalToWorld(1, 1) = 3;
    scene.Primitives[0].LocalToWorld(0, 3) = 5;
    ++scene.Primitives[0].TransformRevision;
    EXPECT_EQ(flights[0].Update(scene), 1u);
    EXPECT_EQ(flights[0].Update(scene), 0u);
    EXPECT_TRUE(static_cast<Eigen::Matrix4f>(AsCBuffer<Forward_ObjectData>(flights[1].Rows().Row(0))->LocalToWorld).isIdentity());
    for (const auto index : {2u, 1u}) {
        EXPECT_EQ(flights[index].Update(scene), 1u);
        const auto* row = AsCBuffer<Forward_ObjectData>(flights[index].Rows().Row(0));
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(row->LocalToWorld).isApprox(scene.Primitives[0].LocalToWorld));
        EXPECT_TRUE(static_cast<Eigen::Matrix4f>(row->NormalToWorld).isApprox(forward_detail::MakeNormalToWorld(scene.Primitives[0].LocalToWorld)));
    }
}

TEST(ForwardObjectValues, SkippedPublicationRepairsValuesBeforeUsingSubsequentChangedRanges) {
    RenderSceneSnapshot scene;
    scene.Valid = true;
    scene.PublicationId = 41;
    scene.PublicationRevision = 1;
    scene.Primitives.push_back({.Generation = 7, .TransformRevision = 1});
    scene.Primitives.push_back({.Generation = 8, .TransformRevision = 1});
    scene.ChangedPrimitiveRanges.push_back({0, 2});
    forward_detail::ForwardObjectDataCache values;
    ASSERT_EQ(values.Update(scene), 2u);

    // The consumer misses a publication, then sees a clean publication of the same flight.
    scene.Primitives[0].LocalToWorld(0, 3) = 31;
    ++scene.Primitives[0].TransformRevision;
    scene.PublicationRevision = 3;
    scene.ChangedFromPublicationRevision = 2;
    scene.ChangedPrimitiveRanges.clear();
    ASSERT_EQ(values.Update(scene), 1u);
    EXPECT_FLOAT_EQ(static_cast<Eigen::Matrix4f>(AsCBuffer<Forward_ObjectData>(values.Rows().Row(0))->LocalToWorld)(0, 3), 31);
    EXPECT_EQ(values.Update(scene), 0u);

    // A continuous publication uses its ranges even when the global Scene epoch jumps.
    scene.SceneEpoch = 99;
    scene.PublicationRevision = 4;
    scene.ChangedFromPublicationRevision = 3;
    scene.Primitives[1].LocalToWorld(2, 2) = -2;
    ++scene.Primitives[1].TransformRevision;
    scene.ChangedPrimitiveRanges.push_back({1, 1});
    EXPECT_EQ(values.Update(scene), 1u);
    EXPECT_TRUE(static_cast<Eigen::Matrix4f>(AsCBuffer<Forward_ObjectData>(values.Rows().Row(1))->NormalToWorld)
                    .isApprox(forward_detail::MakeNormalToWorld(scene.Primitives[1].LocalToWorld)));

    auto detached = scene;
    detached.Primitives[0].LocalToWorld(0, 3) = 44;
    ++detached.Primitives[0].TransformRevision;
    detached.ChangedPrimitiveRanges.clear();
    EXPECT_EQ(values.Update(detached), 1u);
    EXPECT_FLOAT_EQ(static_cast<Eigen::Matrix4f>(AsCBuffer<Forward_ObjectData>(values.Rows().Row(0))->LocalToWorld)(0, 3), 44);
}

TEST(ForwardObjectValues, T36ReorderedAndUnversionedObjectsKeepCorrectDerivedValues) {
    RenderSceneSnapshot scene;
    scene.Primitives.push_back({.Generation = 7, .TransformRevision = 1});
    scene.Primitives.push_back({.Generation = 8, .TransformRevision = 1});
    scene.Primitives[1].LocalToWorld(2, 2) = 0;
    forward_detail::ForwardObjectDataCache values;
    EXPECT_EQ(values.Update(scene), 2u);
    std::swap(scene.Primitives[0], scene.Primitives[1]);
    EXPECT_EQ(values.Update(scene), 2u);
    const auto* singular = AsCBuffer<Forward_ObjectData>(values.Rows().Row(0));
    EXPECT_TRUE(static_cast<Eigen::Matrix4f>(singular->NormalToWorld).allFinite());
    EXPECT_TRUE(static_cast<Eigen::Matrix4f>(singular->NormalToWorld).isApprox(forward_detail::MakeNormalToWorld(scene.Primitives[0].LocalToWorld)));
    scene.Primitives.resize(1);
    EXPECT_EQ(values.Update(scene), 0u);
    scene.Primitives[0].TransformRevision = 0;
    scene.Primitives[0].LocalToWorld(0, 3) = 21;
    EXPECT_EQ(values.Update(scene), 1u);
    EXPECT_FLOAT_EQ(static_cast<Eigen::Matrix4f>(AsCBuffer<Forward_ObjectData>(values.Rows().Row(0))->LocalToWorld)(0, 3), 21);
    scene.Primitives[0].LocalToWorld(0, 3) = 24;
    EXPECT_EQ(values.Update(scene), 1u);
    EXPECT_FLOAT_EQ(static_cast<Eigen::Matrix4f>(AsCBuffer<Forward_ObjectData>(values.Rows().Row(0))->LocalToWorld)(0, 3), 24);
    values.Clear();
    EXPECT_EQ(values.Rows().RowCount(), 0u);
    EXPECT_EQ(values.Update(scene), 1u);
}

Nullable<unique_ptr<ShaderProgram>> CompileProgram(render::Device& device, const std::filesystem::path& path, bool production = false) {
    auto source = ReadBinaryFile(path);
    if (!source) {
        return nullptr;
    }
    ShaderJit jit{{kProjectRoot / "shaderlib"}};
    const auto target = render::GetShaderTargetForBackend(device.GetBackend());
    if (!target || !jit.IsAvailable()) {
        return nullptr;
    }
    const auto contract = jit.DiscoverContractHash("forward_data.hlsl", *source, *target);
    if (!contract) {
        return nullptr;
    }
    shader::CompileVariantRequest request{
        .SourceName = "forward_data.hlsl",
        .RootSource = std::move(*source),
        .Defines = {},
        .Assignments = {},
        .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(*target)),
        .ExpectedContract = *contract};
    if (production) {
        request.Assignments.push_back({.Name = "QUALITY", .Value = "high"});
    }
    auto compiled = jit.Compile(request, *target);
    if (!compiled) {
        return nullptr;
    }
    auto artifact = render::CreateBackendShaderArtifact(
        device, compiled->Metadata, {.Target = *target, .ExpectedGpuArtifact = compiled->ExpectedGpuArtifact},
        ForwardPipeline::GetLayoutRecipe());
    return artifact ? ShaderProgram::Create(&device, std::move(*artifact)) : nullptr;
}

class ImmediateWait final : public IWaitFrameProcessor {
public:
    task<void> Wait() override { co_return; }
};

struct ForwardData {
    render::test::DeviceContext Device;
    unique_ptr<ShaderProgram> Program;
    ImmediateWait Wait;
    AssetManager Assets;
    StreamingAssetRef<TextureAsset> TextureA;
    StreamingAssetRef<TextureAsset> TextureB;
    unique_ptr<MaterialTechnique> Technique;
    unique_ptr<Material> Authoring;
    forward_detail::ForwardProgramBindings Bindings{};

    StreamingAssetRef<TextureAsset> AddTexture(uint32_t id) {
        auto texture = Device.Device->CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = 2, .Height = 2, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Memory = render::MemoryType::Device, .Usage = render::TextureUse::Resource});
        if (!texture.HasValue()) {
            return nullptr;
        }
        auto view = Device.Device->CreateTextureView({.Target = texture.Get(), .Dim = render::TextureDimension::Dim2D, .Format = render::TextureFormat::RGBA8_UNORM, .Range = {0, 1, 0, 1}, .Usage = render::TextureViewUsage::Resource});
        if (!view.HasValue()) {
            return nullptr;
        }
        return Assets.AddReady<TextureAsset>(AssetId{id, 0x1912, 0x4242, 0x88, 1, 2, 3, 4, 5, 6, 7},
                                             make_unique<TextureAsset>(Device.Device.get(), "snapshot texture", texture.Release(), view.Release()));
    }

    bool Initialize() {
        auto program = CompileProgram(*Device.Device, kProjectRoot / "modules/runtime/tests/data/forward_groups.hlsl");
        if (!program.HasValue()) {
            return false;
        }
        Program = program.Release();
        auto bindings = forward_detail::ResolveProgramBindings(*Program);
        if (!bindings) {
            return false;
        }
        Bindings = *bindings;
        Assets.SetWaitFrameProcessor(&Wait);
        TextureA = AddTexture(1);
        TextureB = AddTexture(2);
        auto technique = MaterialTechnique::Create({{"ForwardLit", Program.get(), "ForwardMaterial", {}}}, "ForwardLit");
        if (!technique) return false;
        Technique = technique.Release();
        auto material = Material::Create(Technique.get());
        if (!material.HasValue() || !TextureA.IsReady() || !TextureB.IsReady()) {
            return false;
        }
        Authoring = material.Release();
        const TextureSubViewDesc subview{.Format = render::TextureFormat::RGBA8_UNORM, .Range = {0, 1, 0, 1}};
        render::SamplerDescriptor sampler;
        sampler.MinFilter = render::FilterMode::Linear;
        return Authoring->SetFloat4("ForwardMaterial.BaseColor", Eigen::Vector4f{1, 0, 0, 1}) &&
               Authoring->SetFloat4("Surface", Eigen::Vector4f{0.1f, 0.4f, 0.5f, 0.0f}) &&
               Authoring->SetFloat4("Transmission", Eigen::Vector4f{1, 2, 3, 4}) &&
               Authoring->SetFloat4("UVTransform", Eigen::Vector4f{1, 1, 0, 0}) &&
               Authoring->SetTexture("AlbedoTexture", TextureA, subview) &&
               Authoring->SetSampler("LinearSampler", sampler);
    }
};

template <typename Callback>
void WithForwardData(Callback callback) {
    ForwardData data;
    if (!render::test::TryCreateAnyDevice(data.Device)) {
        GTEST_SKIP() << "No backend available";
    }
    ASSERT_TRUE(data.Initialize());
    callback(data);
}

class ForwardPolicyProxy final : public PrimitiveSceneProxy {
public:
    explicit ForwardPolicyProxy(Material* material) : DrawMaterial(material) {}
    Material* DrawMaterial;
    GpuMesh::DrawData Geometry;
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return 1; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {&Geometry, 0, 3, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
};

TEST(ForwardStaticPolicies, ProductProcessorsReusePublishedStatesAndBindingSchemasAcrossViews) {
    WithForwardData([](ForwardData& data) {
        const auto source = ReadTextFile(kProjectRoot / "shaderlib/pipelines/forward/depth_only.hlsl");
        ASSERT_TRUE(source);
        auto depth = test::CompileStageBProgram(*data.Device.Device, *source, ForwardPipeline::GetDepthOnlyLayoutRecipe());
        ASSERT_TRUE(depth);
        auto technique = MaterialTechnique::Create({{"ForwardLit", data.Program.get(), "ForwardMaterial", {}}, {"DepthOnly", depth.Get(), "", {}}}, "ForwardLit");
        ASSERT_TRUE(technique);
        auto material = Material::Create(technique.Get());
        ASSERT_TRUE(material->SetTexture("AlbedoTexture", data.TextureA));
        ASSERT_TRUE(material->SetSampler("LinearSampler", {}));
        Scene scene;
        auto proxy = make_unique<ForwardPolicyProxy>(material.Get());
        Eigen::Matrix4f mirror = Eigen::Matrix4f::Identity();
        mirror(0, 0) = -1;
        proxy->SetLocalToWorld(mirror);
        scene.AddPrimitive(std::move(proxy));
        AppUpdateContext app{};
        RenderFramePlan plan;
        RenderWorkloadBuilder workloads{plan, {}};
        vector<StreamingAssetRefAny> owners;
        RenderPrepareContext prepare{app, {}, workloads, owners, kPerformanceRenderGraphRuntimeOptions, 0};
        ASSERT_TRUE(forward_detail::RegisterForwardPassPolicies(prepare, scene));
        ASSERT_TRUE(forward_detail::RegisterForwardPassPolicies(prepare, scene));
        ASSERT_EQ(prepare.ScenePolicies.size(), 5u);
        ASSERT_TRUE(prepare.FreezeRegisteredScenes());
        const auto snapshot = prepare.PrepareScene(scene);
        ASSERT_TRUE(snapshot);
        ASSERT_EQ(snapshot->DrawRecords.size(), 3u);
        EXPECT_EQ(snapshot->Stats.BindingRecipeCompiles, 3u);
        PackedCBufferTable objects;
        forward_detail::FreezeObjectData(*snapshot, objects);
        HostWriteBatch writes;
        FrameDrawResources resources{data.Device.Device.get(), {.BasicSize = 16384, .Alignment = 256, .MaxResetSize = 16384}};
        ASSERT_TRUE(resources.BeginFrame(writes));
        forward_detail::ForwardBindingCache forwardBindings;
        forward_detail::DepthOnlyBindingCache depthBindings;
        bool overflow = false;
        forward_detail::ForwardLitMeshPassProcessor forward{resources, forwardBindings, overflow, objects};
        forward_detail::DepthOnlyMeshPassProcessor depthProcessor{resources, depthBindings, objects};
        ResolvedRenderView view;
        CullingResults culling;
        culling.Scene = snapshot.Get();
        culling.View = &view;
        culling.Stats.Valid = true;
        culling.Primitives.push_back({0, 0});
        RendererList normal, readOnly, depthList;
        RendererListDesc desc{"forward", "ForwardLit", &culling, &view};
        for (uint32_t viewIndex = 0; viewIndex < 3; ++viewIndex) {
            view.ViewProjection(0, 3) = static_cast<float>(viewIndex);
            forward.ResetView();
            depthProcessor.ResetView();
            desc.Policy = forward_detail::kForwardLitPolicy;
            ASSERT_TRUE(BuildRendererList(desc, forward, normal));
            desc.Policy = forward_detail::kForwardLitReadOnlyDepthPolicy;
            ASSERT_TRUE(BuildRendererList(desc, forward, readOnly));
            desc.Policy = forward_detail::kDepthOnlyPolicy;
            desc.MaterialPassName = "DepthOnly";
            ASSERT_TRUE(BuildRendererList(desc, depthProcessor, depthList));
            desc.MaterialPassName = "ForwardLit";
            ASSERT_EQ(normal.GetDrawCount(), 1u);
            EXPECT_TRUE(normal.Commands.empty());
            ASSERT_EQ(readOnly.GetDrawCount(), 1u);
            EXPECT_TRUE(readOnly.Commands.empty());
            ASSERT_EQ(depthList.GetDrawCount(), 1u);
            EXPECT_TRUE(depthList.Commands.empty());
            EXPECT_TRUE(normal.GetPipelineState(0).DepthStencil.DepthWriteEnable);
            EXPECT_FALSE(readOnly.GetPipelineState(0).DepthStencil.DepthWriteEnable);
            EXPECT_TRUE(depthList.GetPipelineState(0).DepthStencil.DepthWriteEnable);
            EXPECT_EQ(normal.GetPipelineState(0).Primitive.FaceClockwise,
                      OppositeFrontFace(std::as_const(*material).GetPipelineState().Primitive.FaceClockwise));
            EXPECT_EQ(normal.GetGroups(0).size(), 3u);
            EXPECT_EQ(depthList.GetGroups(0).size(), 2u);
            EXPECT_TRUE(normal.IsCurrent());
            EXPECT_EQ(normal.GetFrameEpoch(), resources.GetEpoch());
            EXPECT_TRUE(normal.GetBindingId(0).IsValid());
            EXPECT_EQ(normal.GetBindingId(0), readOnly.GetBindingId(0));
            EXPECT_NE(normal.GetEffectiveStateId(0), readOnly.GetEffectiveStateId(0));
            EXPECT_EQ(normal.GetEffectiveStateId(0), snapshot->DrawRecords[0].MirroredStateId);
            EXPECT_EQ(&normal.GetDescription(0), &snapshot->DrawRecords[0].Description);
            EXPECT_EQ(normal.GetPrograms().size(), 1u);
            // One material/object plus a distinct Forward/Depth view upload for each actual view.
            EXPECT_EQ(resources.GetStats().SharedBufferUploads, 2u + (viewIndex + 1u) * 2u);
            EXPECT_EQ(resources.GetStats().GroupPreparations, 3u + (viewIndex + 1u) * 2u);
            EXPECT_EQ(forwardBindings.LayoutParses(), 0u);
            EXPECT_EQ(depthBindings.LayoutParses(), 0u);
        }
        // Rebuilding a second processor for the same view uses the frame-owned tuple table.
        const auto groupsBefore = resources.GetGroupCount();
        const auto bindingsBefore = resources.GetBindingCount();
        const auto bytesBefore = resources.GetStats().BufferBytesCopied;
        forward_detail::ForwardLitMeshPassProcessor duplicate{resources, forwardBindings, overflow, objects};
        RendererList shared;
        desc.Policy = forward_detail::kForwardLitPolicy;
        ASSERT_TRUE(BuildRendererList(desc, duplicate, shared));
        EXPECT_EQ(shared.GetBindingId(0), normal.GetBindingId(0));
        EXPECT_EQ(resources.GetGroupCount(), groupsBefore);
        EXPECT_EQ(resources.GetBindingCount(), bindingsBefore);
        EXPECT_EQ(resources.GetStats().BufferBytesCopied, bytesBefore);
        RenderSceneSnapshot changedPublication = *snapshot;
        changedPublication.PublicationId = 71;
        changedPublication.PublicationRevision = 1;
        changedPublication.SceneEpoch = 8;
        culling.Scene = &changedPublication;
        RendererList identityGuard;
        ASSERT_TRUE(BuildRendererList(desc, duplicate, identityGuard));
        EXPECT_TRUE(identityGuard.IsCurrent());
        ++changedPublication.PublicationId;
        EXPECT_FALSE(identityGuard.IsCurrent());
        --changedPublication.PublicationId;
        ++changedPublication.SceneEpoch;
        EXPECT_FALSE(identityGuard.IsCurrent());
        --changedPublication.SceneEpoch;
        ++changedPublication.PublicationRevision;
        EXPECT_FALSE(identityGuard.IsCurrent());
        --changedPublication.PublicationRevision;
        EXPECT_TRUE(identityGuard.IsCurrent());
        RenderSceneSnapshot legacy = *snapshot;
        CpuDrawStore legacyStore;
        ASSERT_TRUE(legacyStore.Sync(legacy));
        EXPECT_FALSE(legacy.HasPassPolicies);
        culling.Scene = &legacy;
        desc.Policy = forward_detail::kForwardLitReadOnlyDepthPolicy;
        ASSERT_TRUE(BuildRendererList(desc, forward, readOnly));
        ASSERT_EQ(readOnly.Commands.size(), 1u);
        EXPECT_FALSE(readOnly.Commands[0].PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_EQ(forwardBindings.LayoutParses(), 1u);
        writes.Flush(*data.Device.Device);
        writes.Reset();
        ASSERT_TRUE(resources.BeginFrame(writes));
        EXPECT_FALSE(normal.IsCurrent());
        EXPECT_FALSE(shared.IsCurrent());
        const auto revision = normal.GetBuildRevision();
        normal.ResetForReuse();
        EXPECT_GT(normal.GetBuildRevision(), revision);
        EXPECT_EQ(normal.GetDrawCount(), 0u);
    });
}

TEST(RadRayRuntimeMaterial, CreateUsesDeclarationAnchor) {
    WithForwardData([](ForwardData& data) {
        EXPECT_EQ(data.Authoring->GetParameterGroup(), data.Bindings.MaterialGroup);
        EXPECT_TRUE(data.Authoring->SetFloat4("ForwardMaterial.BaseColor", Eigen::Vector4f::Ones()));
        EXPECT_FALSE(data.Authoring->SetMatrix4x4("ForwardView.ViewProj", Eigen::Matrix4f::Identity()));
        EXPECT_FALSE(data.Authoring->SetMatrix4x4("ForwardObject.LocalToWorld", Eigen::Matrix4f::Identity()));
        EXPECT_FALSE(data.Authoring->SetFloat4("WrongBuffer.BaseColor", Eigen::Vector4f::Ones()));
        EXPECT_TRUE(data.Authoring->GetParameterStorage().GetBufferData(data.Bindings.ViewBufferIndex).empty());
        EXPECT_TRUE(data.Authoring->GetParameterStorage().GetBufferData(data.Bindings.ObjectBufferIndex).empty());
        auto technique = MaterialTechnique::Create({{"View", data.Program.get(), "ForwardView", {}}}, "View");
        ASSERT_TRUE(technique);
        auto viewMaterial = Material::Create(technique.Get());
        ASSERT_TRUE(viewMaterial.HasValue());
        EXPECT_FALSE(viewMaterial->SetTexture("AlbedoTexture", data.TextureA));
        EXPECT_FALSE(viewMaterial->SetSampler("LinearSampler", {}));
    });
}

TEST(RadRayRuntimeMaterial, UnknownAnchorFails) {
    WithForwardData([](ForwardData& data) {
        EXPECT_FALSE(MaterialTechnique::Create({{"Test", data.Program.get(), "Unknown", {}}}, "Test").HasValue());
        EXPECT_FALSE(MaterialTechnique::Create({{"Test", data.Program.get(), "AlbedoTexture", {}}}, "Test").HasValue());
    });
}

TEST(RadRayRuntimeMaterial, BuildRenderDataCopiesNumericAndResourceState) {
    WithForwardData([](ForwardData& data) {
        data.Authoring->SetRenderQueue(RenderQueue::Transparent);
        data.Authoring->GetPipelineState().DepthStencil.DepthWriteEnable = false;
        data.Authoring->GetPipelineState().Blend = render::BlendState::Default();
        MaterialRenderData snapshot;
        vector<StreamingAssetRefAny> refs;
        ASSERT_TRUE(data.Authoring->BuildRenderData(snapshot, refs));
        EXPECT_EQ(snapshot.Passes.front().Program.Get(), data.Program.get());
        EXPECT_EQ(snapshot.Passes.front().ParameterGroup, data.Bindings.MaterialGroup);
        const auto actual = std::span<const byte>{snapshot.Passes.front().NumericBytes};
        const auto expected = data.Authoring->NumericBytes();
        EXPECT_EQ((vector<byte>{actual.begin(), actual.end()}), (vector<byte>{expected.begin(), expected.end()}));
        EXPECT_NE(actual.data(), expected.data());
        ASSERT_EQ(snapshot.Passes.front().Textures.size(), 1u);
        EXPECT_EQ(snapshot.Passes.front().Textures[0].Texture, data.TextureA.Get().Get());
        EXPECT_EQ(snapshot.Passes.front().Textures[0].Parameter.Binding, data.Program->GetParameterLayout().Find("AlbedoTexture")->Binding);
        EXPECT_EQ(snapshot.Passes.front().Textures[0].Element, 0u);
        EXPECT_FALSE(snapshot.Passes.front().Textures[0].SubView.IsDefault());
        ASSERT_EQ(snapshot.Passes.front().Samplers.size(), 1u);
        EXPECT_EQ(snapshot.Passes.front().Samplers[0].Sampler.MinFilter, render::FilterMode::Linear);
        EXPECT_EQ(snapshot.Queue, RenderQueue::Transparent);
        EXPECT_FALSE(snapshot.Passes.front().PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_EQ(snapshot.Passes.front().PipelineState, data.Authoring->GetPipelineState());
        ASSERT_EQ(refs.size(), 1u);
        EXPECT_EQ(refs.front(), data.TextureA.AsAny());
    });
}

TEST(RadRayRuntimeMaterial, RenderDataDoesNotChangeAfterMaterialMutation) {
    WithForwardData([](ForwardData& data) {
        MaterialRenderData oldData;
        MaterialRenderData newData;
        vector<StreamingAssetRefAny> refs;
        ASSERT_TRUE(data.Authoring->BuildRenderData(oldData, refs));
        const MaterialRenderData saved = oldData;
        ASSERT_TRUE(data.Authoring->SetFloat4("BaseColor", Eigen::Vector4f{0, 1, 0, 1}));
        ASSERT_TRUE(data.Authoring->SetTexture("AlbedoTexture", data.TextureB));
        ASSERT_TRUE(data.Authoring->SetSampler("LinearSampler", {}));
        data.Authoring->SetRenderQueue(RenderQueue::Transparent);
        data.Authoring->GetPipelineState().DepthStencil.DepthWriteEnable = false;
        data.Authoring->GetPipelineState().Blend = render::BlendState::Default();
        ASSERT_TRUE(data.Authoring->BuildRenderData(newData, refs));
        const auto& oldBytes = oldData.Passes.front().NumericBytes;
        const auto& newBytes = newData.Passes.front().NumericBytes;
        const auto& savedBytes = saved.Passes.front().NumericBytes;
        EXPECT_EQ((vector<byte>{oldBytes.begin(), oldBytes.end()}), (vector<byte>{savedBytes.begin(), savedBytes.end()}));
        EXPECT_NE((vector<byte>{oldBytes.begin(), oldBytes.end()}), (vector<byte>{newBytes.begin(), newBytes.end()}));
        EXPECT_NE(oldBytes.data(), newBytes.data());
        EXPECT_EQ(oldData.Passes.front().Textures[0].Texture, data.TextureA.Get().Get());
        EXPECT_EQ(newData.Passes.front().Textures[0].Texture, data.TextureB.Get().Get());
        EXPECT_EQ(oldData.Passes.front().Samplers[0].Sampler, saved.Passes.front().Samplers[0].Sampler);
        EXPECT_NE(oldData.Passes.front().Samplers[0].Sampler, newData.Passes.front().Samplers[0].Sampler);
        EXPECT_EQ(oldData.Queue, RenderQueue::Geometry);
        EXPECT_EQ(newData.Queue, RenderQueue::Transparent);
        EXPECT_TRUE(oldData.Passes.front().PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_FALSE(newData.Passes.front().PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_EQ(oldData.Passes.front().PipelineState, saved.Passes.front().PipelineState);
        EXPECT_EQ(newData.Passes.front().PipelineState, data.Authoring->GetPipelineState());
        EXPECT_NE(oldData.Passes.front().PipelineState.Blend, newData.Passes.front().PipelineState.Blend);
        data.Authoring.reset();
        data.TextureA.Reset();
        data.TextureB.Reset();
        data.Assets.Pump();
        EXPECT_EQ(data.Assets.GetAssetCount(), 2u);
        refs.clear();
        data.Assets.Pump();
        EXPECT_EQ(data.Assets.GetAssetCount(), 0u);
    });
}

TEST(RadRayRuntimeForwardBindings, ResolvesProductionDeclarations) {
    WithForwardData([](ForwardData& data) {
        auto production = CompileProgram(*data.Device.Device, kProjectRoot / "shaderlib/pipelines/forward/forward.hlsl", true);
        ASSERT_TRUE(production.HasValue());
        const auto bindings = forward_detail::ResolveProgramBindings(*production.Get());
        ASSERT_TRUE(bindings.has_value());
        const auto buffers = production->GetParameterLayout().Buffers();
        EXPECT_EQ(buffers[bindings->ViewBufferIndex].Name, "ForwardView");
        EXPECT_EQ(buffers[bindings->MaterialBufferIndex].Name, "ForwardMaterial");
        EXPECT_EQ(buffers[bindings->ObjectBufferIndex].Name, "ForwardObject");
        EXPECT_NE(production->GetParameterLayout().Find("ForwardObject.NormalToWorld"), nullptr);
        EXPECT_EQ(buffers[bindings->ViewBufferIndex].Group, bindings->ViewGroup);
        EXPECT_EQ(buffers[bindings->MaterialBufferIndex].Group, bindings->MaterialGroup);
        EXPECT_EQ(buffers[bindings->ObjectBufferIndex].Group, bindings->ObjectGroup);
    });
}

class SectionComponent final : public PrimitiveComponent {
public:
    SectionComponent(StreamingAssetRef<StaticMesh> mesh, vector<Nullable<Material*>> materials,
                     Eigen::Matrix4f transform)
        : Mesh(std::move(mesh)), Materials(std::move(materials)), Transform(std::move(transform)) {}
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override {
        return make_unique<StaticMeshSceneProxy>(Mesh, Materials, Transform);
    }
    StreamingAssetRef<StaticMesh> Mesh;
    vector<Nullable<Material*>> Materials;
    Eigen::Matrix4f Transform;
};

TEST(RadRayRuntimeForwardPipeline, CollectsEachSectionWithCopiedFacts) {
    WithForwardData([](ForwardData& data) {
        GpuMesh geometry;
        geometry.Draws.emplace_back();
        const AssetId id{3, 0x1912, 0x4242, 0x88, 1, 2, 3, 4, 5, 6, 7};
        auto mesh = data.Assets.AddReady<StaticMesh>(id, make_unique<StaticMesh>(
                                                             MeshResource{}, vector<StaticMeshSection>{{0, 0, 3, 0, 2, 0}, {0, 3, 3, 0, 2, 1}},
                                                             Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones(), std::move(geometry)));
        auto second = Material::Create(data.Technique.get());
        ASSERT_TRUE(second.HasValue());
        ASSERT_TRUE(second->SetTexture("AlbedoTexture", data.TextureB));
        ASSERT_TRUE(second->SetSampler("LinearSampler", {}));
        Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
        transform(0, 3) = 4.0f;
        SectionComponent component{mesh, {data.Authoring.get(), second.Get()}, transform};
        Scene scene;
        auto* proxy = scene.AddPrimitive(component.CreateSceneProxy()).Get();
        ASSERT_NE(proxy, nullptr);
        CameraComponent camera;
        RenderSceneSnapshot input;
        vector<StreamingAssetRefAny> refs;
        ASSERT_TRUE(BuildRenderSceneSnapshot(scene, input, refs));
        scene.RemovePrimitive(proxy);
        component.Transform.setZero();
        ASSERT_EQ(input.MeshBatches.size(), 2u);
        ASSERT_EQ(input.Materials.size(), 2u);
        EXPECT_GE(refs.size(), 3u);
        for (const auto& draw : input.MeshBatches) {
            EXPECT_EQ(draw.Geometry.Get(), &mesh.Get()->GetRenderMesh().Draws[0]);
            EXPECT_EQ(draw.IndexCount, 3u);
            EXPECT_EQ(draw.FirstIndex, draw.SectionIndex * 3);
            EXPECT_EQ(draw.VertexOffset, static_cast<int32_t>(draw.SectionIndex));
            EXPECT_TRUE(input.Primitives[draw.Primitive].LocalToWorld.isApprox(transform));
            const auto* expectedTexture = draw.SectionIndex == 0 ? data.TextureA.Get().Get() : data.TextureB.Get().Get();
            EXPECT_EQ(input.Materials[draw.Material].Passes.front().Textures[0].Texture, expectedTexture);
        }
    });
}

TEST(MaterialTechnique, EquivalentNumericBuffersUseTheirOwnPhysicalGroups) {
    WithForwardData([](ForwardData& data) {
        auto a = test::CompileStageBProgram(*data.Device.Device, test::StageBMaterialSource("float4 BaseColor; float4 Extra[2];", 1));
        auto b = test::CompileStageBProgram(*data.Device.Device, test::StageBMaterialSource("float4 BaseColor; float4 Extra[2];", 6));
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);
        auto technique = MaterialTechnique::Create({{"Primary", a.Get(), "MaterialValues", {}}, {"Secondary", b.Get(), "MaterialValues", {}}}, "Primary");
        ASSERT_TRUE(technique);
        EXPECT_NE(technique->Passes()[0].ParameterGroup, technique->Passes()[1].ParameterGroup);
        auto material = Material::Create(technique.Get());
        ASSERT_TRUE(material->SetFloat4("BaseColor", Eigen::Vector4f{.2f, .3f, .4f, .5f}));
        auto overrideState = material->GetPipelineState();
        overrideState.DepthStencil.DepthWriteEnable = false;
        EXPECT_TRUE(material->SetPassPipelineState("Secondary", overrideState));
        EXPECT_FALSE(material->SetPassPipelineState("Missing", overrideState));
        MaterialRenderData snapshot;
        vector<StreamingAssetRefAny> owners;
        ASSERT_TRUE(material->BuildRenderData(snapshot, owners));
        ASSERT_EQ(snapshot.Passes.size(), 2u);
        EXPECT_TRUE(snapshot.Passes[0].Valid);
        EXPECT_TRUE(snapshot.Passes[1].Valid);
        const auto& first = snapshot.Passes[0].NumericBytes;
        const auto& second = snapshot.Passes[1].NumericBytes;
        EXPECT_EQ((vector<byte>{first.begin(), first.end()}), (vector<byte>{second.begin(), second.end()}));
        EXPECT_NE(first.data(), second.data());
        EXPECT_TRUE(snapshot.Passes[0].PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_FALSE(snapshot.Passes[1].PipelineState.DepthStencil.DepthWriteEnable);
    });
}

TEST(RenderSceneSnapshot, DeduplicatesMaterialsClassifiesSectionsAndRetainsCapacity) {
    WithForwardData([](ForwardData& data) {
        GpuMesh geometry;
        geometry.Draws.emplace_back();
        const AssetId id{4, 0x1912, 0x4242, 0x88, 1, 2, 3, 4, 5, 6, 7};
        auto mesh = data.Assets.AddReady<StaticMesh>(id, make_unique<StaticMesh>(
                                                             MeshResource{}, vector<StaticMeshSection>{{0, 0, 3, 0, 2, 0}, {0, 3, 3, 0, 2, 0}, {2, 0, 3, 0, 2, 0}, {0, 0, 0, 0, 2, 0}, {0, UINT32_MAX - 1, 3, 0, 2, 0}, {0, 0, 3, 0, 2, 3}},
                                                             Eigen::Vector3f::Ones(), Eigen::Vector3f::Zero(), std::move(geometry)));
        SectionComponent component{mesh, {data.Authoring.get(), data.Authoring.get(), data.Authoring.get(), data.Authoring.get(), data.Authoring.get(), nullptr}, Eigen::Matrix4f::Identity()};
        Scene scene;
        auto* proxy = scene.AddPrimitive(component.CreateSceneProxy()).Get();
        ASSERT_NE(proxy, nullptr);
        RenderSceneSnapshot snapshot;
        vector<StreamingAssetRefAny> retained;
        ASSERT_TRUE(BuildRenderSceneSnapshot(scene, snapshot, retained));
        EXPECT_EQ(snapshot.Stats.InputSections, 6u);
        EXPECT_EQ(snapshot.Stats.MissingGeometry, 1u);
        EXPECT_EQ(snapshot.Stats.EmptyDraw, 1u);
        EXPECT_EQ(snapshot.Stats.InvalidDrawRange, 1u);
        EXPECT_EQ(snapshot.Stats.MaterialUnavailable, 1u);
        EXPECT_EQ(snapshot.Stats.InputMaterials, 1u);
        EXPECT_EQ(snapshot.Stats.InvalidBounds, 1u);
        ASSERT_EQ(snapshot.MeshBatches.size(), 2u);
        EXPECT_EQ(snapshot.Materials.size(), 1u);
        EXPECT_EQ(snapshot.MeshBatches[0].Material, snapshot.MeshBatches[1].Material);
        EXPECT_EQ(snapshot.Primitives[0].FirstMeshBatch, 0u);
        EXPECT_EQ(snapshot.Primitives[0].MeshBatchCount, 2u);
        const auto capacity = snapshot.MeshBatches.capacity();
        scene.RemovePrimitive(proxy);
        ASSERT_TRUE(BuildRenderSceneSnapshot(scene, snapshot, retained));
        EXPECT_TRUE(snapshot.Primitives.empty());
        EXPECT_TRUE(snapshot.MeshBatches.empty());
        EXPECT_TRUE(snapshot.Materials.empty());
        EXPECT_EQ(snapshot.MeshBatches.capacity(), capacity);
        EXPECT_EQ(snapshot.Stats.BatchHighWatermark, 2u);
        EXPECT_EQ(snapshot.Stats.MaterialHighWatermark, 1u);
        EXPECT_EQ(snapshot.Stats.InputSections, 0u);
    });
}

TEST(MaterialTechnique, InvalidNamesAndAnchorsFailClosed) {
    WithForwardData([](ForwardData& data) {
        auto a = test::CompileStageBProgram(*data.Device.Device, test::StageBMaterialSource());
        ASSERT_TRUE(a);
        const MaterialPassDesc primary{"Primary", a.Get(), "MaterialValues", {}};
        EXPECT_FALSE(MaterialTechnique::Create({primary, primary}, "Primary"));
        EXPECT_FALSE(MaterialTechnique::Create({primary}, "Absent"));
        EXPECT_FALSE(MaterialTechnique::Create({{"Primary", nullptr, "MaterialValues", {}}}, "Primary"));
        EXPECT_FALSE(MaterialTechnique::Create({{"Primary", a.Get(), "Missing", {}}}, "Primary"));
        EXPECT_FALSE(MaterialTechnique::Create({{"Primary", a.Get(), "", {}}}, "Primary"));
    });
}

// Create only needs the primary cbuffer's byte count to size the authoring storage. Numeric
// layout agreement across passes is the caller's contract, kept by sharing one generated
// header and one HLSL struct, so a differing secondary layout is no longer rejected here.
TEST(MaterialTechnique, SecondaryNumericLayoutIsTheCallersContract) {
    WithForwardData([](ForwardData& data) {
        auto a = test::CompileStageBProgram(*data.Device.Device, test::StageBMaterialSource());
        ASSERT_TRUE(a);
        const MaterialPassDesc primary{"Primary", a.Get(), "MaterialValues", {}};
        for (const auto fields : {"float3 BaseColor;", "float4 Padding; float4 BaseColor;", "float4 BaseColor; float4 Extra[2];"}) {
            SCOPED_TRACE(fields);
            auto b = test::CompileStageBProgram(*data.Device.Device, test::StageBMaterialSource(fields, 3));
            ASSERT_TRUE(b);
            EXPECT_TRUE(MaterialTechnique::Create({primary, {"Secondary", b.Get(), "MaterialValues", {}}}, "Primary"));
        }
    });
}

TEST(MaterialTechnique, ResourceSubsetIsValidButAdditionalResourceFails) {
    WithForwardData([](ForwardData& data) {
        auto full = test::CompileStageBProgram(*data.Device.Device, test::StageBMaterialSource("float4 BaseColor;", 1, true, true));
        auto subset = test::CompileStageBProgram(*data.Device.Device, test::StageBMaterialSource("float4 BaseColor;", 6, true, false));
        ASSERT_TRUE(full);
        ASSERT_TRUE(subset);
        auto technique = MaterialTechnique::Create({{"Primary", full.Get(), "MaterialValues", {}}, {"Secondary", subset.Get(), "MaterialValues", {}}}, "Primary");
        ASSERT_TRUE(technique);
        EXPECT_FALSE(MaterialTechnique::Create({{"Primary", subset.Get(), "MaterialValues", {}}, {"Secondary", full.Get(), "MaterialValues", {}}}, "Primary"));
        auto material = Material::Create(technique.Get());
        EXPECT_TRUE(material->SetTexture("AlbedoTexture", data.TextureA));
        EXPECT_TRUE(material->SetSampler("LinearSampler", {}));
        MaterialRenderData snapshot;
        vector<StreamingAssetRefAny> owners;
        ASSERT_TRUE(material->BuildRenderData(snapshot, owners));
        EXPECT_FALSE(snapshot.FindPass("Primary")->Valid);
        EXPECT_TRUE(snapshot.FindPass("Secondary")->Valid);
        EXPECT_EQ(snapshot.FindPass("Secondary")->Textures.size(), 1u);
        EXPECT_TRUE(material->SetTexture("NormalTexture", data.TextureB));
        EXPECT_FALSE(snapshot.FindPass("Primary")->Valid);
        ASSERT_TRUE(material->BuildRenderData(snapshot, owners));
        EXPECT_TRUE(snapshot.FindPass("Primary")->Valid);
    });
}

TEST(MaterialTechnique, MissingTextureLeavesDepthOnlyValidWithoutMaterialBinding) {
    WithForwardData([](ForwardData& data) {
        const auto source = ReadTextFile(kProjectRoot / "shaderlib/pipelines/forward/depth_only.hlsl");
        ASSERT_TRUE(source);
        auto depth = test::CompileStageBProgram(*data.Device.Device, *source, ForwardPipeline::GetDepthOnlyLayoutRecipe());
        ASSERT_TRUE(depth);
        auto technique = MaterialTechnique::Create({{"ForwardLit", data.Program.get(), "ForwardMaterial", {}}, {"DepthOnly", depth.Get(), "", {}}}, "ForwardLit");
        ASSERT_TRUE(technique);
        auto material = Material::Create(technique.Get());
        MaterialRenderData snapshot;
        vector<StreamingAssetRefAny> owners;
        ASSERT_TRUE(material->BuildRenderData(snapshot, owners));
        EXPECT_FALSE(snapshot.FindPass("ForwardLit")->Valid);
        EXPECT_TRUE(snapshot.FindPass("DepthOnly")->Valid);
        EXPECT_FALSE(snapshot.FindPass("DepthOnly")->ParameterGroup);
        EXPECT_TRUE(snapshot.FindPass("DepthOnly")->NumericBytes.empty());
        EXPECT_TRUE(owners.empty());
        EXPECT_TRUE(forward_detail::ResolveDepthOnlyProgramBindings(*depth.Get()));
    });
}

TEST(FrameDrawResources, DynamicOffsetsReuseImmutableSetsAndSpillsCreateNewSets) {
    WithForwardData([](ForwardData& data) {
        HostWriteBatch writes;
        DynamicCBufferArena::Descriptor descriptor{.BasicSize = 1024, .Alignment = 256, .MaxResetSize = 1024};
        FrameDrawResources resources{data.Device.Device.get(), descriptor};
        ASSERT_TRUE(resources.BeginFrame(writes));
        MaterialRenderData snapshot;
        vector<StreamingAssetRefAny> owners;
        ASSERT_TRUE(data.Authoring->BuildRenderData(snapshot, owners));
        const auto& pass = snapshot.Passes.front();
        const auto first = resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, std::span<const byte>{pass.NumericBytes}, pass.Textures, pass.Samplers);
        const auto second = resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, std::span<const byte>{pass.NumericBytes}, pass.Textures, pass.Samplers);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        EXPECT_EQ(first->Set.Get(), second->Set.Get());
        EXPECT_NE(first->DynamicOffsets, second->DynamicOffsets);
        EXPECT_EQ(resources.GetSetCount(), 1u);
        bool spilled = false;
        for (uint32_t i = 0; i < 10; ++i) {
            const auto group = resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, std::span<const byte>{pass.NumericBytes}, pass.Textures, pass.Samplers);
            ASSERT_TRUE(group);
            spilled |= group->Set.Get() != first->Set.Get();
        }
        EXPECT_TRUE(spilled);
        EXPECT_GT(resources.GetSetCount(), 1u);
        EXPECT_EQ(resources.GetStats().RecipeBuilds, 1u);
        EXPECT_EQ(resources.GetStats().GroupPreparations, 12u);
        EXPECT_EQ(resources.GetStats().SetCreations + resources.GetStats().SetCacheHits, 12u);
        EXPECT_EQ(resources.GetStats().BufferBytesCopied, 12u * pass.NumericBytes.size());
        EXPECT_FALSE(resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, ShaderParameterStorage{}, pass.Textures, pass.Samplers));
        EXPECT_FALSE(resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, std::span<const byte>{pass.NumericBytes}));
        writes.Flush(*data.Device.Device);
        writes.Reset();
        ASSERT_TRUE(resources.BeginFrame(writes));
        EXPECT_EQ(resources.GetSetCount(), 0u);
        const auto* recipe = &pass.Program->GetOrCreateParameterGroupRecipe(*pass.ParameterGroup);
        const auto next = resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, std::span<const byte>{pass.NumericBytes}, pass.Textures, pass.Samplers);
        ASSERT_TRUE(next);
        EXPECT_EQ(resources.GetStats().RecipeBuilds, 0u);
        EXPECT_EQ(resources.GetStats().SetCreations, 1u);
        EXPECT_EQ(resources.GetStats().BufferBytesCopied, pass.NumericBytes.size());
        HostWriteBatch otherWrites;
        FrameDrawResources otherFlight{data.Device.Device.get(), descriptor};
        ASSERT_TRUE(otherFlight.BeginFrame(otherWrites));
        const auto other = otherFlight.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, std::span<const byte>{pass.NumericBytes}, pass.Textures, pass.Samplers);
        ASSERT_TRUE(other);
        EXPECT_NE(other->Set.Get(), next->Set.Get());
        EXPECT_EQ(otherFlight.GetStats().RecipeBuilds, 0u);
        EXPECT_EQ(&pass.Program->GetOrCreateParameterGroupRecipe(*pass.ParameterGroup), recipe);
        writes.Flush(*data.Device.Device);
        otherWrites.Flush(*data.Device.Device);
    });
}

TEST(FrameDrawResources, SharedObjectUploadsKeepWireViewAndNativeLayoutIdentitySeparate) {
    WithForwardData([](ForwardData& data) {
        auto second = CompileProgram(*data.Device.Device, kProjectRoot / "modules/runtime/tests/data/forward_groups.hlsl");
        ASSERT_TRUE(second);
        const auto secondBinding = forward_detail::ResolveProgramBindings(*second.Get());
        ASSERT_TRUE(secondBinding);
        EXPECT_NE(second->GetGeneration(), data.Program->GetGeneration());
        HostWriteBatch writes;
        FrameDrawResources resources{data.Device.Device.get(), {.BasicSize = 1024, .Alignment = 256, .MaxResetSize = 1024}};
        ASSERT_TRUE(resources.BeginFrame(writes));
        Forward_ObjectData object{};
        object.LocalToWorld = Eigen::Matrix4f{Eigen::Matrix4f::Identity()};
        static constexpr byte wireA{}, wireB{};
        const FrameCBufferIdentity shared{&object, &wireA, 0};
        const auto bytes = AsCBufferBytes(object);
        const auto first = resources.PrepareSharedCBufferGroup(*data.Program, data.Bindings.ObjectGroup, shared, 0, bytes);
        const auto duplicate = resources.PrepareSharedCBufferGroup(*data.Program, data.Bindings.ObjectGroup, shared, 0, bytes);
        const auto otherProgram = resources.PrepareSharedCBufferGroup(*second.Get(), secondBinding->ObjectGroup, shared, 0, bytes);
        ASSERT_TRUE(first);
        ASSERT_TRUE(duplicate);
        ASSERT_TRUE(otherProgram);
        EXPECT_EQ(first->Set, duplicate->Set);
        EXPECT_EQ(first->DynamicOffsets, duplicate->DynamicOffsets);
        EXPECT_NE(first->Set, otherProgram->Set);
        ASSERT_EQ(first->DynamicOffsets.size(), 1u);
        ASSERT_EQ(otherProgram->DynamicOffsets.size(), 1u);
        EXPECT_EQ(first->DynamicOffsets[0].Offset, otherProgram->DynamicOffsets[0].Offset);
        EXPECT_EQ(first->DynamicOffsets[0].Binding, data.Program->GetParameterLayout().Buffers()[data.Bindings.ObjectBufferIndex].Binding);
        EXPECT_EQ(otherProgram->DynamicOffsets[0].Binding, second->GetParameterLayout().Buffers()[secondBinding->ObjectBufferIndex].Binding);
        EXPECT_NE(first->DynamicOffsets[0].Binding, otherProgram->DynamicOffsets[0].Binding);
        EXPECT_EQ(resources.GetStats().BufferBytesCopied, sizeof(object));
        EXPECT_EQ(resources.GetStats().SharedBufferUploads, 1u);
        EXPECT_EQ(resources.GetStats().SharedBufferHits, 2u);
        EXPECT_EQ(resources.GetStats().SharedGroupHits, 1u);
        EXPECT_EQ(resources.GetStats().GroupPreparations, 2u);
        const auto indexed = resources.PrepareSharedCBufferGroupId(*data.Program, data.Bindings.ObjectGroup, shared, 0, bytes);
        ASSERT_TRUE(indexed.IsValid());
        EXPECT_EQ(resources.GetGroup(indexed).Set, first->Set);
        EXPECT_EQ(resources.GetStats().GroupPreparations, 2u);
        for (const FrameCBufferIdentity identity : {FrameCBufferIdentity{&object, &wireB, 0}, FrameCBufferIdentity{&object, &wireA, 17}}) {
            const auto distinct = resources.PrepareSharedCBufferGroup(*data.Program, data.Bindings.ObjectGroup, identity, 0, bytes);
            ASSERT_TRUE(distinct);
            EXPECT_NE(first->DynamicOffsets, distinct->DynamicOffsets);
        }
        EXPECT_EQ(resources.GetStats().SharedBufferUploads, 3u);
        bool spilled = false;
        for (uint32_t row = 1; row < 20; ++row) {
            const auto value = resources.PrepareSharedCBufferGroup(*data.Program, data.Bindings.ObjectGroup, shared, row, bytes);
            ASSERT_TRUE(value);
            spilled |= value->Set != first->Set;
        }
        EXPECT_TRUE(spilled);
        const auto stillFirst = resources.PrepareSharedCBufferGroup(*data.Program, data.Bindings.ObjectGroup, shared, 0, bytes);
        ASSERT_TRUE(stillFirst);
        EXPECT_EQ(stillFirst->Set, first->Set);
        EXPECT_EQ(stillFirst->DynamicOffsets, first->DynamicOffsets);
        EXPECT_EQ(resources.GetGroup(indexed).Set, first->Set);
        EXPECT_EQ(resources.GetGroup(indexed).DynamicOffsets, first->DynamicOffsets);
        writes.Flush(*data.Device.Device);
        writes.Reset();
        ASSERT_TRUE(resources.BeginFrame(writes));
        const auto nextFrame = resources.PrepareSharedCBufferGroup(*data.Program, data.Bindings.ObjectGroup, shared, 0, bytes);
        ASSERT_TRUE(nextFrame);
        EXPECT_EQ(resources.GetStats().SharedBufferUploads, 1u);
        EXPECT_EQ(resources.GetStats().SharedBufferHits, 0u);
        EXPECT_EQ(resources.GetStats().SharedGroupHits, 0u);
        writes.Flush(*data.Device.Device);
        writes.Reset();
        ASSERT_TRUE(resources.BeginFrame(writes));
        object.MotionValid = 1;
        const auto firstDomain = resources.PrepareSharedCBufferGroupId(*second.Get(), secondBinding->ObjectGroup,
                                                                       {&object, &wireB, 17}, 0, bytes);
        const auto secondDomain = resources.PrepareSharedCBufferGroupId(*second.Get(), secondBinding->ObjectGroup, shared, 0, bytes);
        const auto reorderedProgram = resources.PrepareSharedCBufferGroupId(*data.Program, data.Bindings.ObjectGroup, shared, 0, bytes);
        ASSERT_TRUE(firstDomain.IsValid());
        ASSERT_TRUE(secondDomain.IsValid());
        ASSERT_TRUE(reorderedProgram.IsValid());
        EXPECT_NE(resources.GetGroup(firstDomain).DynamicOffsets[0].Offset, resources.GetGroup(secondDomain).DynamicOffsets[0].Offset);
        EXPECT_EQ(resources.GetGroup(secondDomain).DynamicOffsets[0].Offset, resources.GetGroup(reorderedProgram).DynamicOffsets[0].Offset);
        EXPECT_EQ(resources.GetGroup(reorderedProgram).DynamicOffsets[0].Binding, data.Program->GetParameterLayout().Buffers()[data.Bindings.ObjectBufferIndex].Binding);
        EXPECT_EQ(resources.GetStats().SharedBufferUploads, 2u);
        EXPECT_EQ(resources.GetStats().SharedBufferHits, 1u);
        EXPECT_EQ(resources.GetStats().SharedGroupHits, 0u);
        writes.Flush(*data.Device.Device);
    });
}

TEST(FrameDrawResources, IndexedTuplesPublishOnlySuccessAndRetryWithoutUploadingAgain) {
    WithForwardData([](ForwardData& data) {
        HostWriteBatch writes;
        FrameDrawResources resources{data.Device.Device.get()};
        ASSERT_TRUE(resources.BeginFrame(writes));
        Forward_ObjectData object{};
        static constexpr byte wire{};
        FrameCBufferIdentity identity{&object, &wire, 0, 1, 2, 3};
        resources.FailNextGroupForTesting();
        const auto failed = resources.PrepareGroupId(*data.Program, data.Bindings.ObjectGroup, identity, 0, AsCBufferBytes(object));
        EXPECT_FALSE(failed.IsValid());
        EXPECT_EQ(resources.GetGroupCount(), 0u);
        EXPECT_EQ(resources.GetBindingCount(), 0u);
        EXPECT_EQ(resources.GetStats().SharedBufferUploads, 1u);
        const auto retry = resources.PrepareGroupId(*data.Program, data.Bindings.ObjectGroup, identity, 0, {});
        ASSERT_TRUE(retry.IsValid());
        EXPECT_EQ(resources.GetGroupCount(), 1u);
        EXPECT_EQ(resources.GetStats().SharedBufferUploads, 1u);
        EXPECT_EQ(resources.GetStats().BufferBytesCopied, sizeof(object));
        const array groups{retry};
        const auto first = resources.InternBinding(groups);
        ASSERT_TRUE(first.IsValid());
        EXPECT_EQ(resources.InternBinding(groups), first);
        EXPECT_EQ(resources.GetBindingCount(), 1u);
        EXPECT_FALSE(resources.InternBinding(array{FrameShaderGroupId{}}).IsValid());
        const auto original = resources.GetGroup(retry).DynamicOffsets;
        for (uint32_t row = 1; row < 500; ++row) {
            const auto next = resources.PrepareGroupId(*data.Program, data.Bindings.ObjectGroup, identity, row, AsCBufferBytes(object));
            ASSERT_TRUE(next.IsValid());
            ASSERT_TRUE(resources.InternBinding(array{next}).IsValid());
        }
        EXPECT_EQ(resources.GetGroup(resources.GetBinding(first)[0]).DynamicOffsets, original);
        // Equal byte count is insufficient: each temporal or lifetime dimension creates a new row.
        for (uint32_t dimension = 0; dimension < 4; ++dimension) {
            auto changed = identity;
            if (dimension == 0) ++changed.Context;
            if (dimension == 1) ++changed.Generation;
            if (dimension == 2) ++changed.Revision;
            if (dimension == 3) ++changed.HistoryRevision;
            const auto next = resources.PrepareGroupId(*data.Program, data.Bindings.ObjectGroup, changed, 0, AsCBufferBytes(object));
            ASSERT_TRUE(next.IsValid());
            EXPECT_NE(resources.GetGroup(next).DynamicOffsets, original);
        }
        writes.Flush(*data.Device.Device);
    });
}

TEST(FrameDrawResources, EpochTablesRetainWarmStorageWithoutKeepingHistoricalKeys) {
    WithForwardData([](ForwardData& data) {
        HostWriteBatch writes;
        FrameDrawResources resources{data.Device.Device.get(), {.BasicSize = 65536, .Alignment = 256, .MaxResetSize = 65536}};
        Forward_ObjectData object{};
        Forward_ViewData view{};
        static constexpr byte objectWire{}, viewWire{};
        size_t warmedBytes = 0;
        uint64_t epoch = 0;
        for (uint64_t frame = 0; frame < 40; ++frame) {
            ASSERT_TRUE(resources.BeginFrame(writes));
            EXPECT_GT(resources.GetEpoch(), epoch);
            epoch = resources.GetEpoch();
            EXPECT_EQ(resources.GetGroupCount(), 0u);
            EXPECT_EQ(resources.GetBindingCount(), 0u);
            view.ViewProj.m[0] = static_cast<float>(frame + 1);
            const auto values = resources.InternValues(&viewWire, AsCBufferBytes(view));
            const auto viewId = resources.PrepareGroupId(*data.Program, data.Bindings.ViewGroup, values, 0, AsCBufferBytes(view));
            ASSERT_TRUE(viewId.IsValid());
            for (uint32_t row = 0; row < 64; ++row) {
                const auto objectId = resources.PrepareGroupId(*data.Program, data.Bindings.ObjectGroup,
                                                               {&object, &objectWire, frame + 1}, row, AsCBufferBytes(object));
                ASSERT_TRUE(objectId.IsValid());
                const array groups{viewId, objectId};
                const auto tuple = resources.InternBinding(groups);
                ASSERT_TRUE(tuple.IsValid());
                for (uint32_t repeat = 0; repeat < 3; ++repeat) EXPECT_EQ(resources.InternBinding(groups), tuple);
            }
            EXPECT_EQ(resources.GetGroupCount(), 65u);
            EXPECT_EQ(resources.GetBindingCount(), 64u);
            EXPECT_EQ(resources.GetStats().SharedBufferUploads, 65u);
            if (frame == 2) warmedBytes = resources.GetCacheCapacityBytes();
            if (frame > 2) EXPECT_EQ(resources.GetCacheCapacityBytes(), warmedBytes);
            writes.Flush(*data.Device.Device);
            writes.Reset();
        }
    });
}

}  // namespace
}  // namespace radray
