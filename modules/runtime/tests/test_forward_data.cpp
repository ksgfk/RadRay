#include "runtime_test_support.h"
#include "stage_b_test_support.h"
#include "gpu_test_fixture.h"
#include "forward_pipeline/forward_bindings.h"
#include "forward_pipeline/forward_frame.h"

#include <gtest/gtest.h>

#include <radray/file.h>
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
               Authoring->SetFloat("Roughness", 0.4f) &&
               Authoring->SetFloat2("Tint", Eigen::Vector2f{0.2f, 0.3f}) &&
               Authoring->SetFloat3("NormalBias", Eigen::Vector3f{1, 2, 3}) &&
               Authoring->SetMatrix4x4("MaterialTransform", Eigen::Matrix4f::Identity()) &&
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
        const auto actual = snapshot.Passes.front().Parameters.GetBufferData(data.Bindings.MaterialBufferIndex);
        const auto expected = data.Authoring->GetParameterStorage().GetBufferData(data.Bindings.MaterialBufferIndex);
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
        auto oldBytes = oldData.Passes.front().Parameters.GetBufferData(data.Bindings.MaterialBufferIndex);
        auto newBytes = newData.Passes.front().Parameters.GetBufferData(data.Bindings.MaterialBufferIndex);
        auto savedBytes = saved.Passes.front().Parameters.GetBufferData(data.Bindings.MaterialBufferIndex);
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
        auto* proxy = scene.AddPrimitive(&component);
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
        auto first = snapshot.Passes[0].Parameters.GetBufferData(*technique->Passes()[0].BufferIndex);
        auto second = snapshot.Passes[1].Parameters.GetBufferData(*technique->Passes()[1].BufferIndex);
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
        auto* proxy = scene.AddPrimitive(&component);
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

TEST(MaterialTechnique, InvalidNamesAnchorsAndNumericLayoutsFailClosed) {
    WithForwardData([](ForwardData& data) {
        auto a = test::CompileStageBProgram(*data.Device.Device, test::StageBMaterialSource());
        ASSERT_TRUE(a);
        const MaterialPassDesc primary{"Primary", a.Get(), "MaterialValues", {}};
        EXPECT_FALSE(MaterialTechnique::Create({primary, primary}, "Primary"));
        EXPECT_FALSE(MaterialTechnique::Create({primary}, "Absent"));
        EXPECT_FALSE(MaterialTechnique::Create({{"Primary", nullptr, "MaterialValues", {}}}, "Primary"));
        EXPECT_FALSE(MaterialTechnique::Create({{"Primary", a.Get(), "Missing", {}}}, "Primary"));
        EXPECT_FALSE(MaterialTechnique::Create({{"Primary", a.Get(), "", {}}}, "Primary"));
        for (const auto fields : {"float3 BaseColor;", "float4 Padding; float4 BaseColor;", "float4 BaseColor; float4 Extra[2];"}) {
            SCOPED_TRACE(fields);
            auto b = test::CompileStageBProgram(*data.Device.Device, test::StageBMaterialSource(fields, 3));
            ASSERT_TRUE(b);
            EXPECT_FALSE(MaterialTechnique::Create({primary, {"Secondary", b.Get(), "MaterialValues", {}}}, "Primary"));
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
        EXPECT_EQ(snapshot.FindPass("DepthOnly")->Parameters.GetLayout(), nullptr);
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
        const auto first = resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, pass.Parameters, pass.Textures, pass.Samplers);
        const auto second = resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, pass.Parameters, pass.Textures, pass.Samplers);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        EXPECT_EQ(first->Set.Get(), second->Set.Get());
        EXPECT_NE(first->DynamicOffsets, second->DynamicOffsets);
        EXPECT_EQ(resources.GetSetCount(), 1u);
        bool spilled = false;
        for (uint32_t i = 0; i < 10; ++i) {
            const auto group = resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, pass.Parameters, pass.Textures, pass.Samplers);
            ASSERT_TRUE(group);
            spilled |= group->Set.Get() != first->Set.Get();
        }
        EXPECT_TRUE(spilled);
        EXPECT_GT(resources.GetSetCount(), 1u);
        EXPECT_FALSE(resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, ShaderParameterStorage{}, pass.Textures, pass.Samplers));
        EXPECT_FALSE(resources.PrepareGroup(*pass.Program.Get(), *pass.ParameterGroup, pass.Parameters));
        writes.Flush(*data.Device.Device);
        writes.Reset();
        ASSERT_TRUE(resources.BeginFrame(writes));
        EXPECT_EQ(resources.GetSetCount(), 0u);
    });
}

}  // namespace
}  // namespace radray
