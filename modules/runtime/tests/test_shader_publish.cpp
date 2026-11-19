#include <gtest/gtest.h>

#include "test_asset_fakes.h"
#include "../../../examples/mesh_drawing_pipeline/pipeline.h"

namespace radray {
namespace {
using namespace examples::mesh_drawing;
class PublishDevice final : public test::LayoutDevice {
public:
    Nullable<unique_ptr<gpu::GraphicsPipelineState>> CreateGraphicsPipelineState(const gpu::GraphicsPipelineStateDescriptor&) noexcept override {
        if (Fail) return nullptr;
        return make_unique<test::FakePipeline>();
    }
    bool Fail{};
};

ShaderAssetDesc CubeFixture() {
    using shader::ShaderScalarKind;
    using shader::ShaderTypeKind;
    auto layout = test::MatrixLayout();
    auto& group = layout.Groups[0];
    group.GroupSlot = 0;
    auto& binding = group.Bindings[0];
    binding.Name = "Pass";
    binding.Binding.BindingId = 0;
    binding.Binding.Stages = gpu::ShaderStage::Vertex;
    binding.Binding.D3D12 = gpu::D3D12BindingDescriptor{gpu::BindingRegisterClass::CBV, 0};
    binding.BufferByteSize = 80;
    layout.Types = {
        {.Name = "PassConstants", .Size = 80, .ElementSize = 80},
        {.Name = "ViewProjection", .ParentIndex = 0, .Kind = ShaderTypeKind::Matrix, .ElementKind = ShaderTypeKind::Matrix, .Size = 64, .ElementSize = 64, .MatrixStride = 16, .ScalarKind = ShaderScalarKind::Float, .RowCount = 4, .ColumnCount = 4},
        {.Name = "LocalScale", .ParentIndex = 0, .Kind = ShaderTypeKind::Scalar, .ElementKind = ShaderTypeKind::Scalar, .Offset = 64, .Size = 4, .ElementSize = 4, .ScalarKind = ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 1},
        {.Name = "Padding", .ParentIndex = 0, .Kind = ShaderTypeKind::Vector, .ElementKind = ShaderTypeKind::Vector, .Offset = 68, .Size = 12, .ElementSize = 12, .ScalarKind = ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 3},
        {.Name = "MaterialPushData", .Size = 16, .ElementSize = 16},
        {.Name = "Color", .ParentIndex = 4, .Kind = ShaderTypeKind::Vector, .ElementKind = ShaderTypeKind::Vector, .Size = 16, .ElementSize = 16, .ScalarKind = ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 4}};
    layout.PushConstants.push_back({"MaterialPush", {.PushId = 0, .Size = 16, .Stages = gpu::ShaderStage::Vertex | gpu::ShaderStage::Pixel, .D3D12 = gpu::PushConstantDescriptor::D3D12Location{1, 0}}, 4});
    auto desc = test::MakeShaderDesc(std::move(layout));
    desc.Family.Variants[0].DXIL->ActiveBindings = {{0, 0}};
    desc.Family.Variants[0].DXIL->ActivePushConstants = {0};
    return desc;
}

class ShaderPublish : public testing::Test {
protected:
    void SetUp() override {
        Resources.Device = make_unique<DeviceContext>(Device, &Device->Queue);
        Resources.LayoutRegistry = make_unique<PipelineLayoutRegistry>(*Device);
        Resources.Assets = make_unique<AssetManager>();
        Resources.Shader = test::CreateTestShader(*Resources.Assets, *Device, CubeFixture());
        ASSERT_TRUE(Resources.Shader);
        Resources.OpaqueTag = {*Resources.MaterialTags.Intern("Surface"), *Resources.MaterialTags.Intern("Opaque")};
        Desc = test::MakeMaterialDesc(Resources.Shader, {{"Color", array<float, 4>{1, 0, 0, 1}}});
        Desc.Properties.Tags = MaterialTagSet{Resources.MaterialTags};
        ASSERT_TRUE(Desc.Properties.Tags.Set(Resources.OpaqueTag.Key, Resources.OpaqueTag.Value));
        Material = CreateMaterial(*Resources.Assets, Desc);
        ASSERT_TRUE(Material);
        ASSERT_TRUE(Resources.CreatePipelines(Input, Material));
        MaterialRenderProxy proxy;
        ASSERT_TRUE(proxy.Update(*Resources.Assets, Material.GetHandle()));
        Proxies[0] = World.AddMaterial(std::move(proxy));
        auto primitive = World.AddPrimitive();
        MeshBatch batch;
        batch.Material = Proxies[0];
        batch.VertexInput = &Input;
        batch.Elements.push_back({.IndexBuffer = &Indices, .IndexCount = 3});
        Policies[0] = {World.AddMeshBatch(primitive, std::move(batch)), PassParticipation::DepthAndColor, "fixture"};
        Processor = make_unique<DepthPassProcessor>(World, *Resources.Assets, Resources.Pipelines, Policies,
                                                    MaterialTagFilter{&Resources.MaterialTags, Resources.OpaqueTag, "opaque"});
        Scheduler = test::CreateScheduler(*Resources.Device, Callbacks, 2);
        ASSERT_TRUE(Scheduler);
        Callbacks->Passes = {{{0}, Processor.get()}};
        Callbacks->OnRecord = [&](FrameSlot& slot) {
            test::RecordingEncoder encoder;
            MeshPassFrameDrawList list;
            EXPECT_TRUE(World.GatherVisibleMeshDrawCommands({0}, array{VisibleMeshBatch{.Batch = Policies[0].Batch}}, list));
            EXPECT_TRUE(list.Submit(encoder));
            Draws = encoder.Draws.size();
            Bound = encoder.Pipelines.empty() ? nullptr : encoder.Pipelines.front();
            EXPECT_TRUE(slot.AcquireCommandBuffer());
        };
    }
    AssetCommitResult ReplaceShader(ShaderAssetDesc desc, bool rebuildMaterial = false) {
        auto shader = ShaderAsset::Create(*Device, desc);
        if (!shader) return {};
        AssetTransaction tx(*Resources.Assets);
        tx.StageReplace(Resources.Shader, std::move(*shader));
        if (rebuildMaterial) StageMaterial(tx, Material, Desc);
        return Resources.Assets->Commit(std::move(tx), *Scheduler->BeginFrame().Emplace<AssetGarbageBatch>());
    }
    AssetCommitResult ReplaceMaterial() {
        AssetTransaction tx(*Resources.Assets);
        StageMaterial(tx, Material, Desc);
        return Resources.Assets->Commit(std::move(tx), *Scheduler->BeginFrame().Emplace<AssetGarbageBatch>());
    }
    void Frame(std::span<const AssetReloadEvent> events = {}) {
        Resources.SyncPipelines(Input, events, World, Proxies, Scheduler->BeginFrame());
        World.SyncCachedMeshDrawCommands(Callbacks->Passes);
        Scheduler->RunFrame();
    }
    shared_ptr<PublishDevice> Device{make_shared<PublishDevice>()};
    GpuResources Resources;
    MaterialAssetDesc Desc;
    AssetRef<MaterialAsset> Material;
    VertexInput Input;
    test::FakeBuffer Indices{64, gpu::BufferUse::Index};
    Scene World;
    array<MaterialProxyHandle, 1> Proxies{};
    array<BatchPassPolicy, 1> Policies{};
    unique_ptr<DepthPassProcessor> Processor;
    Nullable<gpu::GraphicsPipelineState*> Bound{nullptr};
    size_t Draws{};
    test::CallbackPipeline* Callbacks{};
    unique_ptr<FrameScheduler> Scheduler;
};

TEST_F(ShaderPublish, CompatibleRetryRefreshesResidentCommandsWithoutAnotherEvent) {
    Frame();
    ASSERT_EQ(Draws, 1u);
    const auto old = Bound;
    const auto result = ReplaceShader(CubeFixture());
    ASSERT_TRUE(result.Succeeded);
    Device->Fail = true;
    Frame(result.Events);
    EXPECT_EQ(Bound, old);
    Device->Fail = false;
    Frame();
    const auto current = Resources.Pipelines[0].Depth.Get();
    EXPECT_NE(current, old.Get());
    EXPECT_EQ(Bound.Get(), current);
    Frame();
    Frame();
    EXPECT_EQ(Draws, 1u);
    EXPECT_EQ(Bound.Get(), current);
}

TEST_F(ShaderPublish, BreakingFailureStaysPausedAcrossCompatibleEventUntilSuccess) {
    Frame();
    Desc.Properties.RenderState.Cull = gpu::CullMode::Front;
    const auto breaking = ReplaceMaterial();
    ASSERT_TRUE(breaking.Succeeded);
    Device->Fail = true;
    Frame(breaking.Events);
    EXPECT_EQ(Draws, 0u);
    const auto compatible = ReplaceMaterial();
    ASSERT_TRUE(compatible.Succeeded);
    Frame(compatible.Events);
    EXPECT_EQ(Draws, 0u);
    Device->Fail = false;
    Frame();
    EXPECT_EQ(Draws, 1u);
    EXPECT_EQ(Resources.Pipelines[0].AppliedMaterialRevision, 3u);
}

TEST_F(ShaderPublish, TagReloadChangesDepthParticipation) {
    Frame();
    ASSERT_EQ(Draws, 1u);
    const auto setTag = [&](MaterialTagId value) {
        ASSERT_TRUE(Desc.Properties.Tags.Set(Resources.OpaqueTag.Key, value));
        const auto result = ReplaceMaterial();
        ASSERT_TRUE(result.Succeeded);
        ASSERT_EQ(result.Events.size(), 1u);
        EXPECT_EQ(result.Events.front().Kind, AssetReloadKind::Breaking);
        Frame(result.Events);
    };
    setTag(*Resources.MaterialTags.Intern("Transparent"));
    EXPECT_EQ(Draws, 0u);
    setTag(Resources.OpaqueTag.Value);
    EXPECT_EQ(Draws, 1u);
}

TEST_F(ShaderPublish, MissingEventPausesAndFixedPassAbiMustMatchToResume) {
    Frame();
    ASSERT_TRUE(ReplaceShader(CubeFixture()).Succeeded);
    Device->Fail = true;
    Frame();
    EXPECT_EQ(Draws, 0u);
    Device->Fail = false;
    auto changed = CubeFixture();
    changed.Family.SharedLayout.Types[2].Offset = 76;
    changed.Family.SharedLayout.Types[3].Offset = 64;
    auto result = ReplaceShader(std::move(changed), true);
    ASSERT_TRUE(result.Succeeded);
    Frame(result.Events);
    EXPECT_EQ(Draws, 0u);
    EXPECT_EQ(Resources.Pipelines[0].AppliedShaderRevision, 1u);
    result = ReplaceShader(CubeFixture(), true);
    ASSERT_TRUE(result.Succeeded);
    Frame(result.Events);
    EXPECT_EQ(Draws, 1u);
}

TEST_F(ShaderPublish, ExtraActiveResourcePausesDrawsUntilPassCanSupplyItsBindings) {
    Frame();
    auto changed = CubeFixture();
    auto group = test::ResourceLayout().Groups.front();
    group.GroupSlot = 1;
    group.Bindings.resize(1);
    group.Bindings[0].Binding.Vulkan.reset();
    changed.Family.SharedLayout.Groups.push_back(std::move(group));
    changed.Family.Variants[0].DXIL->ActiveBindings.push_back({1, 0});
    const auto result = ReplaceShader(std::move(changed), true);
    ASSERT_TRUE(result.Succeeded);
    Frame(result.Events);
    EXPECT_EQ(Draws, 0u);
    EXPECT_EQ(Resources.Pipelines[0].AppliedShaderRevision, 1u);
}

TEST_F(ShaderPublish, ProxyRevisionRefreshesInheritedValuesAndPreservesOverrides) {
    array instances{MaterialInstance{Material.GetHandle()}, MaterialInstance{Material.GetHandle()}};
    const array<float, 4> overrideColor{0, 1, 0, 1};
    ASSERT_TRUE(instances[1].SetParameter(*Resources.Assets, 0, overrideColor));
    array proxies{Proxies[0], World.AddMaterial({})};
    array<uint32_t, 2> applied{};
    ASSERT_TRUE(SyncMaterialProxies(*Resources.Assets, World, instances, proxies, applied));
    for (float value : {2.0f, 3.0f}) {
        const array<float, 4> defaultColor{0, 0, value, 1};
        Desc.Parameters[0].DefaultValue = defaultColor;
        const auto result = ReplaceMaterial();
        ASSERT_TRUE(result.Succeeded);
        if (value == 2.0f)
            Frame(result.Events);
        else
            Frame();
        ASSERT_TRUE(SyncMaterialProxies(*Resources.Assets, World, instances, proxies, applied));
        EXPECT_EQ((std::get<array<float, 4>>(*World.GetMaterial(proxies[0])->GetParameter(0))), defaultColor);
        EXPECT_EQ((std::get<array<float, 4>>(*World.GetMaterial(proxies[1])->GetParameter(0))), overrideColor);
    }
}
}  // namespace
}  // namespace radray
