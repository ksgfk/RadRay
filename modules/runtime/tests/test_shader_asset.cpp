#include <gtest/gtest.h>
#include <chrono>
#include <radray/file.h>
#include <radray/json.h>
#include <radray/runtime/frame_scheduler.h>
#include <radray/runtime/shader_asset.h>
#include "test_asset_fakes.h"

namespace radray {
namespace {
using namespace test;
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

TEST(ShaderAsset, ExplicitShutdownRetainsShaderUntilBothSlotsReset) {
    auto device = make_shared<LayoutDevice>();
    DeviceContext context{device, &device->Queue};
    auto fallback = ShaderAsset::Create(*device, MakeShaderDesc());
    ASSERT_TRUE(fallback);
    AssetManager assets;
    assets.RegisterLoader<ShaderAsset, ShaderAssetLoader>(*device, &*fallback);
    const auto id = MakeAssetId("pending");
    ASSERT_TRUE(assets.RegisterMeta<ShaderAsset>({id, {}, {}}));
    auto pending = assets.Request<ShaderAsset>(id);
    const auto borrowed = pending.Resolve();
    ASSERT_EQ(borrowed.Get(), &*fallback);
    auto shader = CreateTestShader(assets, *device, MakeShaderDesc());
    ASSERT_TRUE(shader);
    CallbackPipeline* pipeline{nullptr};
    auto scheduler = CreateScheduler(context, pipeline, 2);
    ASSERT_TRUE(scheduler);
    pipeline->OnPrepare = [&](FrameSlot& slot) {
        slot.Emplace<AssetRef<ShaderAsset>>(shader);
        slot.Emplace<AssetRef<ShaderAsset>>(pending);
        EXPECT_TRUE(slot.AcquireCommandBuffer());
    };
    pipeline->OnWait = [&](FrameSlot&) { EXPECT_TRUE(borrowed->GetStage(0, gpu::ShaderStage::Vertex)); };
    scheduler->RunFrame();
    scheduler->RunFrame();
    pipeline->OnPrepare = {};
    shader.Reset();
    pending.Reset();
    assets.Shutdown();
    EXPECT_EQ(assets.CollectGarbage(), 0u);
    scheduler->RunFrame();
    EXPECT_EQ(assets.CollectGarbage(), 0u);
    EXPECT_EQ(device->ShaderDestroys, 0u);
    scheduler->BeginFrame();
    EXPECT_EQ(assets.CollectGarbage(), 2u);
    EXPECT_EQ(device->ShaderDestroys, 2u);
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
        Assets.RegisterLoader<ShaderAsset, ShaderAssetLoader>(Device);
        ASSERT_TRUE(Assets.RegisterMeta<ShaderAsset>({MakeAssetId("shader.index.json"), Directory / "shader.index.json", {}}));
    }
    void TearDown() override {
        std::error_code error;
        if (Directory.parent_path() == std::filesystem::temp_directory_path()) std::filesystem::remove_all(Directory, error);
    }
    AssetManager Assets;
    bool Load() {
        const auto id = MakeAssetId("shader.index.json");
        Assets.CollectGarbage();
        auto ref = Assets.Request<ShaderAsset>(id);
        Assets.Pump();
        return ref.IsReady();
    }
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
