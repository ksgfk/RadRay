#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <radray/file.h>
#include <radray/hash.h>
#include <radray/json.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/shader_asset.h>
#include "test_asset_fakes.h"

namespace radray {
namespace {
using namespace test;
TEST(ShaderAsset, CanonicalUriFindsNamedAsset) {
    AssetManager assets;
    auto uri = Uri::TryCreate("virtual://shaders/example");
    ASSERT_TRUE(uri);
    auto ref = assets.Emplace(42, {}, MakeAssetId(*uri));
    EXPECT_EQ(assets.Get(assets.Find<int>(MakeAssetId("VIRTUAL://SHADERS/./example"))), ref.Get());
    EXPECT_EQ(MakeAssetId("named").Value, HashData64("named", 5));
    EXPECT_FALSE(MakeAssetId("://bad").IsValid());
}

TEST(ShaderAsset, EmptyFamilyCannotPublishAnAsset) {
    LayoutDevice device;
    AssetManager assets;
    EXPECT_FALSE(CreateTestShader(assets, device, {}));
    EXPECT_FALSE(assets.Get(AssetHandle<ShaderAsset>{}));
}
TEST(ShaderAsset, CreationAndQueriesDoNotCreateNativeLayouts) {
    LayoutDevice device;
    AssetManager assets;
    auto desc = MakeShaderDesc(MaterialLayout());
    desc.Family.SharedLayout.Groups[0].Bindings[0].Binding.D3D12.reset();
    const auto handle = CreateTestShader(assets, device, desc);
    ASSERT_TRUE(handle);
    const auto asset = handle.Get();
    EXPECT_EQ(device.ShaderCreates, 2u);
    EXPECT_EQ(device.GroupCreates, 0u);
    EXPECT_EQ(device.LayoutCreates, 0u);
    const auto constants = asset->FindBinding("Constants");
    ASSERT_TRUE(constants);
    const auto& group = desc.Family.SharedLayout.Groups.front();
    EXPECT_EQ(constants->Reference.GroupSlot, group.GroupSlot);
    EXPECT_EQ(constants->Reference.BindingId, group.Bindings.front().Binding.BindingId);
    EXPECT_FALSE(constants->ActiveOnDevice);
    EXPECT_EQ(asset->FindBufferMember("Constants", "Tint")->AbsoluteOffset, 16u);
    EXPECT_EQ(asset->GetStage(0, gpu::ShaderStage::Vertex)->EntryPoint, "VS");
    EXPECT_FALSE(asset->GetStage(0, gpu::ShaderStage::Compute));
    EXPECT_FALSE(asset->GetVariantInterface(1));
}
TEST(ShaderAsset, InvalidStagesFailBeforeGpuCreationAndPartialCreationIsReleased) {
    LayoutDevice device;
    AssetManager assets;
    auto desc = MakeShaderDesc();
    desc.Stages.pop_back();
    EXPECT_FALSE(CreateTestShader(assets, device, desc));
    EXPECT_EQ(device.ShaderCreates, 0u);
    desc = MakeShaderDesc();
    desc.Stages.push_back(desc.Stages.front());
    EXPECT_FALSE(CreateTestShader(assets, device, desc));
    EXPECT_EQ(device.ShaderCreates, 0u);
    desc = MakeShaderDesc();
    device.FailShaderAt = 2;
    EXPECT_FALSE(CreateTestShader(assets, device, desc));
    EXPECT_EQ(device.ShaderDestroys, 1u);
    device.FailShaderAt = 0;
    const auto handle = CreateTestShader(assets, device, desc);
    ASSERT_TRUE(handle);
    EXPECT_EQ(handle.GetHandle().GetIndex(), 0u);
}
TEST(ShaderAsset, ClassifiesLayoutProgramAndDeviceChanges) {
    LayoutDevice device, otherDevice;
    AssetManager assets;
    auto desc = MakeShaderDesc();
    desc.Family.Variants.push_back(desc.Family.Variants.front());
    desc.Family.Variants.back().Name = "Second";
    auto stages = desc.Stages;
    for (auto stage : stages) {
        stage.VariantIndex = 1;
        desc.Stages.push_back(std::move(stage));
    }
    auto target = CreateTestShader(assets, device, desc);
    ASSERT_TRUE(target);
    auto candidate = ShaderAsset::Create(device, desc);
    ASSERT_TRUE(candidate);
    auto reordered = desc;
    std::swap(reordered.Family.Variants[0], reordered.Family.Variants[1]);
    auto changed = ShaderAsset::Create(device, reordered);
    ASSERT_TRUE(changed);
    EXPECT_EQ(target.Get()->ClassifyReload(*changed).Kind, AssetReloadKind::Breaking);
    auto renamed = desc;
    renamed.Stages[0].Entry = "changed";
    changed = ShaderAsset::Create(device, renamed);
    ASSERT_TRUE(changed);
    EXPECT_EQ(target.Get()->ClassifyReload(*changed).Kind, AssetReloadKind::Breaking);
    auto layoutChange = desc;
    layoutChange.Family.SharedLayout = MaterialLayout();
    changed = ShaderAsset::Create(device, layoutChange);
    ASSERT_TRUE(changed);
    EXPECT_EQ(target.Get()->ClassifyReload(*changed).Kind, AssetReloadKind::Breaking);
    auto other = ShaderAsset::Create(otherDevice, desc);
    ASSERT_TRUE(other);
    EXPECT_EQ(target.Get()->ClassifyReload(*other).Kind, AssetReloadKind::Rejected);
    EXPECT_EQ(target.Get()->ClassifyReload(*candidate).Kind, AssetReloadKind::Compatible);
}

TEST(ShaderAsset, OwnersSurviveEveryFrameSlotWaitAndCommandReset) {
    auto device = make_shared<LayoutDevice>();
    DeviceContext context{device, &device->Queue};
    auto assets = make_unique<AssetManager>();
    auto handle = CreateTestShader(*assets, *device, MakeShaderDesc());
    ASSERT_TRUE(handle);
    CallbackPipeline* pipeline{nullptr};
    auto scheduler = CreateScheduler(context, pipeline, 3);
    ASSERT_TRUE(scheduler);
    uint32_t waits = 0, resets = 0;
    pipeline->OnPrepare = [&](FrameSlot& slot) {
        EXPECT_TRUE(handle.Get());
        EXPECT_EQ(device->ShaderDestroys, 0u);
        EXPECT_TRUE(slot.AcquireCommandBuffer());
    };
    pipeline->OnWait = [&](FrameSlot&) {
        ++waits;
        EXPECT_EQ(device->ShaderDestroys, 0u);
    };
    device->OnAllocatorReset = [&] {
        ++resets;
        EXPECT_EQ(device->ShaderDestroys, 0u);
    };
    for (uint32_t frame = 0; frame != 5; ++frame) scheduler->RunFrame();
    EXPECT_EQ(waits, 2u);
    scheduler->WaitIdle();
    EXPECT_EQ(waits, 5u);
    EXPECT_GE(resets, 5u);
    scheduler.reset();
    handle.Reset();
    assets.reset();
    EXPECT_EQ(device->ShaderDestroys, 2u);
}

TEST(ShaderAsset, BorrowedTextureViewRetiresInTheReloadSlotAcrossFailedRetries) {
    vector<string> trace;
    vector<gpu::Texture*> recorded;
    FakeGpu gpu;
    gpu.Device->EnableTextures = true;
    gpu.Device->OnViewDestroyed = [&] { trace.push_back("view"); };
    gpu.Device->OnTextureDestroyed = [&] { trace.push_back("texture"); };
    gpu.Device->OnAllocatorReset = [&] { trace.push_back("reset"); };
    AssetManager assets;
    auto texture = assets.Emplace(gpu.Device->CreateTexture({}).Release());
    auto view = gpu.Device->CreateTextureView({.Target = texture.Get()->get()});
    ASSERT_TRUE(view);
    uint32_t applied = assets.GetRevision(texture.Untyped());
    CallbackPipeline* pipeline{};
    auto scheduler = CreateScheduler(gpu.Context, pipeline, 2);
    ASSERT_TRUE(scheduler);
    pipeline->OnWait = [&](FrameSlot&) { trace.push_back("wait"); };
    pipeline->OnRecord = [&](FrameSlot& slot) {
        EXPECT_TRUE(slot.AcquireCommandBuffer());
        if (view) recorded.push_back(view->GetDesc().Target);
    };
    scheduler->RunFrame();
    ASSERT_EQ(recorded.size(), 1u);
    auto& reloadSlot = scheduler->BeginFrame();
    AssetTransaction tx(assets);
    ASSERT_TRUE(tx.StageReplace(texture, gpu.Device->CreateTexture({}).Release()));
    ASSERT_TRUE(assets.Commit(std::move(tx), *reloadSlot.Emplace<AssetGarbageBatch>()).Succeeded);
    reloadSlot.Emplace<unique_ptr<gpu::TextureView>>(view.Release());
    const auto sync = [&] {
        const auto revision = assets.GetRevision(texture.Untyped());
        if (applied == revision) return;
        view = gpu.Device->CreateTextureView({.Target = texture.Get()->get()});
        if (view) applied = revision;
    };
    gpu.Device->FailViews = true;
    sync();
    scheduler->RunFrame();
    scheduler->BeginFrame();
    sync();
    scheduler->RunFrame();
    EXPECT_EQ(recorded.size(), 1u);
    EXPECT_EQ(applied, 1u);
    EXPECT_EQ(std::count(trace.begin(), trace.end(), "view"), 0);
    trace.clear();
    scheduler->BeginFrame();
    EXPECT_EQ(trace, (vector<string>{"wait", "reset", "view", "texture"}));
    gpu.Device->FailViews = false;
    sync();
    scheduler->RunFrame();
    EXPECT_EQ(applied, 2u);
    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_EQ(recorded.back(), texture.Get()->get());
}

class ShaderAssetFiles : public testing::Test {
protected:
    std::filesystem::path Directory;
    LayoutDevice Device;
    void SetUp() override {
        Directory = std::filesystem::temp_directory_path() / radray::format("radray-asset-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::error_code error;
        ASSERT_TRUE(std::filesystem::create_directory(Directory, error));
        ASSERT_FALSE(error);
    }
    void TearDown() override {
        std::error_code error;
        if (Directory.parent_path() == std::filesystem::temp_directory_path()) std::filesystem::remove_all(Directory, error);
    }
    bool Load() { return ShaderAsset::Load(Device, Directory / "shader.index.json").has_value(); }
    void Write(bool duplicate = false, bool missing = false, uint32_t version = 1) {
        auto desc = MakeShaderDesc(MaterialLayout());
        desc.Family.Variants[0].SPIRV = desc.Family.Variants[0].DXIL;
        const auto layout = shader::WriteShaderFamilyLayoutBinary(desc.Family);
        ASSERT_TRUE(layout);
        ASSERT_TRUE(WriteBinaryFile(Directory / "layout.bin", *layout));
        JsonWriter writer;
        auto root = writer.RootObject();
        root.AddUint("version", version);
        root.AddString("layoutBinary", "layout.bin");
        auto stages = root.AddArray("stages");
        for (uint32_t i = 0; i < (duplicate ? 3u : missing ? 1u
                                                           : 2u);
             ++i) {
            auto stage = stages.AppendObject();
            stage.AddString("variant", "Default");
            stage.AddString("backend", "dxil");
            stage.AddString("stage", i == 1 ? "pixel" : "vertex");
            stage.AddString("entry", i == 1 ? "PS" : "VS");
            stage.AddString("file", i == 1 ? "ps.bin" : "vs.bin");
        }
        auto ignored = stages.AppendObject();
        ignored.AddString("variant", "Default");
        ignored.AddString("backend", "spirv");
        ignored.AddString("stage", "vertex");
        ignored.AddString("entry", "VS");
        ignored.AddString("file", "not-distributed.bin");
        ASSERT_TRUE(writer.WriteFile(Directory / "shader.index.json"));
        ASSERT_TRUE(WriteBinaryFile(Directory / "vs.bin", desc.Stages[0].Bytecode));
        ASSERT_TRUE(WriteBinaryFile(Directory / "ps.bin", desc.Stages[1].Bytecode));
    }
};
TEST_F(ShaderAssetFiles, CompileInMemoryWithDiskIncludesAndVariantIndices) {
    const auto uri = Uri::TryCreate("virtual://shaders/generated.hlsl");
    ASSERT_TRUE(uri);
    ASSERT_TRUE(WriteTextFile(Directory / "position.hlsli", "float4 position(float3 p) {return float4(p,1);}"));
    constexpr std::string_view source = R"(
#include <position.hlsli>
[shader("vertex")] float4 VSMain(float3 p : POSITION) : SV_Position {return position(p);}
)";
    array directories{Directory};
    ShaderCompileDesc desc{.SourceUri = *uri, .Source = std::as_bytes(std::span{source}),
                           .IncludePaths = directories, .Variants = {{"Second", {}}, {"First", {}}}};
    shader::Client compiler;
    for (const auto backend : {gpu::RenderBackend::D3D12, gpu::RenderBackend::Vulkan}) {
        Device.Backend = backend;
        auto result = ShaderAsset::Compile(Device, compiler, desc);
        ASSERT_TRUE(result.Succeeded()) << result.Diagnostics;
        EXPECT_EQ(result.Value->GetBackend(), backend);
        EXPECT_EQ(result.Value->GetFamilyDesc().Variants.front().Name, "Second");
        ASSERT_TRUE(result.Value->GetStage(1, gpu::ShaderStage::Vertex));
        EXPECT_EQ(result.Value->GetVariantInterface(0)->VertexInputs.size(), 1u);
    }
    const auto expectFailure = [&](const shader::Client& client) {
        auto result = ShaderAsset::Compile(Device, client, desc);
        EXPECT_FALSE(result.Succeeded());
        EXPECT_FALSE(result.Diagnostics.empty());
    };
    shader::Client missing{"radray_missing_shader_compiler_probe"};
    expectFailure(missing);
    desc.IncludePaths = {};
    expectFailure(compiler);
    constexpr std::string_view invalid = "invalid HLSL!";
    desc.Source = std::as_bytes(std::span{invalid});
    expectFailure(compiler);
    desc.SourceUri = {};
    expectFailure(compiler);
}

TEST_F(ShaderAssetFiles, LoadsRelativePathsWithoutOtherBackendFilesOrLayouts) {
    Write();
    ASSERT_TRUE(Load());
    EXPECT_EQ(Device.ShaderCreates, 2u);
    EXPECT_EQ(Device.LayoutCreates, 0u);
}
TEST_F(ShaderAssetFiles, RejectsDuplicateMissingStagesAndUnsupportedIndex) {
    Write(true);
    EXPECT_FALSE(Load());
    Write(false, true);
    EXPECT_FALSE(Load());
    Write(false, false, 2);
    EXPECT_FALSE(Load());
    EXPECT_EQ(Device.ShaderCreates, 0u);
}
}  // namespace
}  // namespace radray
