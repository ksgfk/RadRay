#include "runtime_test_support.h"
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
#include <radray/runtime/shader_jit.h>

namespace radray {
namespace {

const std::filesystem::path kProjectRoot{RADRAY_PROJECT_DIR};

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
        auto material = Material::Create(Program.get(), "ForwardMaterial");
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
        auto viewMaterial = Material::Create(data.Program.get(), "ForwardView");
        ASSERT_TRUE(viewMaterial.HasValue());
        EXPECT_FALSE(viewMaterial->SetTexture("AlbedoTexture", data.TextureA));
        EXPECT_FALSE(viewMaterial->SetSampler("LinearSampler", {}));
    });
}

TEST(RadRayRuntimeMaterial, UnknownAnchorFails) {
    WithForwardData([](ForwardData& data) {
        EXPECT_FALSE(Material::Create(data.Program.get(), "Unknown").HasValue());
        EXPECT_FALSE(Material::Create(data.Program.get(), "AlbedoTexture").HasValue());
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
        EXPECT_EQ(snapshot.Program.Get(), data.Program.get());
        EXPECT_EQ(snapshot.ParameterGroup, data.Bindings.MaterialGroup);
        const auto actual = snapshot.Parameters.GetBufferData(data.Bindings.MaterialBufferIndex);
        const auto expected = data.Authoring->GetParameterStorage().GetBufferData(data.Bindings.MaterialBufferIndex);
        EXPECT_EQ((vector<byte>{actual.begin(), actual.end()}), (vector<byte>{expected.begin(), expected.end()}));
        EXPECT_NE(actual.data(), expected.data());
        ASSERT_EQ(snapshot.Textures.size(), 1u);
        EXPECT_EQ(snapshot.Textures[0].Texture, data.TextureA.Get().Get());
        EXPECT_EQ(snapshot.Textures[0].Parameter.Binding, data.Program->GetParameterLayout().Find("AlbedoTexture")->Binding);
        EXPECT_EQ(snapshot.Textures[0].Element, 0u);
        EXPECT_FALSE(snapshot.Textures[0].SubView.IsDefault());
        ASSERT_EQ(snapshot.Samplers.size(), 1u);
        EXPECT_EQ(snapshot.Samplers[0].Sampler.MinFilter, render::FilterMode::Linear);
        EXPECT_EQ(snapshot.Queue, RenderQueue::Transparent);
        EXPECT_FALSE(snapshot.PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_EQ(snapshot.PipelineState, data.Authoring->GetPipelineState());
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
        auto oldBytes = oldData.Parameters.GetBufferData(data.Bindings.MaterialBufferIndex);
        auto newBytes = newData.Parameters.GetBufferData(data.Bindings.MaterialBufferIndex);
        auto savedBytes = saved.Parameters.GetBufferData(data.Bindings.MaterialBufferIndex);
        EXPECT_EQ((vector<byte>{oldBytes.begin(), oldBytes.end()}), (vector<byte>{savedBytes.begin(), savedBytes.end()}));
        EXPECT_NE((vector<byte>{oldBytes.begin(), oldBytes.end()}), (vector<byte>{newBytes.begin(), newBytes.end()}));
        EXPECT_NE(oldBytes.data(), newBytes.data());
        EXPECT_EQ(oldData.Textures[0].Texture, data.TextureA.Get().Get());
        EXPECT_EQ(newData.Textures[0].Texture, data.TextureB.Get().Get());
        EXPECT_EQ(oldData.Samplers[0].Sampler, saved.Samplers[0].Sampler);
        EXPECT_NE(oldData.Samplers[0].Sampler, newData.Samplers[0].Sampler);
        EXPECT_EQ(oldData.Queue, RenderQueue::Geometry);
        EXPECT_EQ(newData.Queue, RenderQueue::Transparent);
        EXPECT_TRUE(oldData.PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_FALSE(newData.PipelineState.DepthStencil.DepthWriteEnable);
        EXPECT_EQ(oldData.PipelineState, saved.PipelineState);
        EXPECT_EQ(newData.PipelineState, data.Authoring->GetPipelineState());
        EXPECT_NE(oldData.PipelineState.Blend, newData.PipelineState.Blend);
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
        auto second = Material::Create(data.Program.get(), "ForwardMaterial");
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
        forward_detail::ForwardFrameInput input;
        vector<StreamingAssetRefAny> refs;
        forward_detail::CollectFrameInput(&scene, &camera, input, refs);
        scene.RemovePrimitive(proxy);
        component.Transform.setZero();
        ASSERT_EQ(input.Draws.size(), 2u);
        ASSERT_EQ(input.Materials.size(), 2u);
        EXPECT_GE(refs.size(), 3u);
        for (const auto& draw : input.Draws) {
            EXPECT_EQ(draw.Geometry, &mesh.Get()->GetRenderMesh().Draws[0]);
            EXPECT_EQ(draw.IndexCount, 3u);
            EXPECT_EQ(draw.FirstIndex, draw.SectionIndex * 3);
            EXPECT_EQ(draw.VertexOffset, static_cast<int32_t>(draw.SectionIndex));
            EXPECT_TRUE(draw.LocalToWorld.isApprox(transform));
            const auto* expectedTexture = draw.SectionIndex == 0 ? data.TextureA.Get().Get() : data.TextureB.Get().Get();
            EXPECT_EQ(input.Materials[draw.MaterialIndex].Textures[0].Texture, expectedTexture);
        }
    });
}

}  // namespace
}  // namespace radray
