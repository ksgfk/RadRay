#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <radray/file.h>
#include <radray/json.h>
#include <radray/runtime/frame_scheduler.h>
#include "test_asset_fakes.h"

namespace radray {
namespace {
using namespace test;
TEST(ShaderAsset, EmptyFamilyCannotPublishAnAsset) {
    LayoutDevice device;
    AssetManager assets{device};
    EXPECT_FALSE(assets.CreateShader({}));
    EXPECT_FALSE(assets.Get(AssetHandle<ShaderAsset>{}));
}
TEST(ShaderAsset, CreationAndQueriesDoNotCreateNativeLayouts) {
    LayoutDevice device;
    AssetManager assets{device};
    auto desc = MakeShaderDesc(MaterialLayout());
    desc.Family.SharedLayout.Groups[0].Bindings[0].Binding.D3D12.reset();
    const auto handle = assets.CreateShader(desc);
    ASSERT_TRUE(handle);
    const auto asset = assets.Get(*handle);
    EXPECT_EQ(device.ShaderCreates, 2u);
    EXPECT_EQ(device.GroupCreates, 0u);
    EXPECT_EQ(device.LayoutCreates, 0u);
    EXPECT_FALSE(asset->FindBinding("Constants")->ActiveOnDevice);
    EXPECT_EQ(asset->FindBufferMember("Constants", "Tint")->AbsoluteOffset, 16u);
    EXPECT_EQ(asset->GetStage(0, render::ShaderStage::Vertex)->EntryPoint, "VS");
    EXPECT_FALSE(asset->GetStage(0, render::ShaderStage::Compute));
    EXPECT_FALSE(asset->GetVariantInterface(1));
}
TEST(ShaderAsset, InvalidStagesFailBeforeGpuCreationAndPartialCreationIsReleased) {
    LayoutDevice device;
    AssetManager assets{device};
    auto desc = MakeShaderDesc();
    desc.Stages.pop_back();
    EXPECT_FALSE(assets.CreateShader(desc));
    EXPECT_EQ(device.ShaderCreates, 0u);
    desc = MakeShaderDesc();
    desc.Stages.push_back(desc.Stages.front());
    EXPECT_FALSE(assets.CreateShader(desc));
    EXPECT_EQ(device.ShaderCreates, 0u);
    desc = MakeShaderDesc();
    device.FailShaderAt = 2;
    EXPECT_FALSE(assets.CreateShader(desc));
    EXPECT_EQ(device.ShaderDestroys, 1u);
    device.FailShaderAt = 0;
    const auto handle = assets.CreateShader(desc);
    ASSERT_TRUE(handle);
    EXPECT_EQ(handle->GetIndex(), 0u);
}
TEST(ShaderAsset, OwnersSurviveEveryFrameSlotWaitAndCommandReset) {
    auto device = make_shared<LayoutDevice>();
    DeviceContext context{device, &device->Queue};
    auto assets = make_unique<AssetManager>(*device);
    const auto handle = assets->CreateShader(MakeShaderDesc());
    ASSERT_TRUE(handle);
    CallbackPipeline* pipeline{nullptr};
    auto scheduler = CreateScheduler(context, pipeline, 3);
    ASSERT_TRUE(scheduler);
    uint32_t waits = 0, resets = 0;
    pipeline->OnPrepare = [&](FrameSlot& slot) {
        EXPECT_TRUE(assets->Get(*handle));
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
    assets.reset();
    EXPECT_EQ(device->ShaderDestroys, 2u);
}

TEST(PipelineLayoutCache, OwnsKeysAndReusesLayoutsAcrossFamiliesAndOrdering) {
    LayoutDevice device;
    PipelineLayoutCache cache{device};
    render::PipelineLayout* first;
    {
        const auto desc = ResourceLayout();
        const auto result = cache.Acquire(desc);
        ASSERT_TRUE(result);
        first = result.Get();
    }
    auto desc = ResourceLayout();
    desc.Groups[0].Name = "Renamed";
    desc.Groups[0].Bindings[0].Name = "Other";
    desc.Groups[0].Bindings[0].Binding.Vulkan->Binding = 99;
    std::reverse(desc.Groups[0].Bindings.begin(), desc.Groups[0].Bindings.end());
    EXPECT_EQ(cache.Acquire(desc).Get(), first);
    EXPECT_EQ(device.GroupCreates, 1u);
    EXPECT_EQ(device.LayoutCreates, 1u);
    desc.Groups[0].GroupSlot = 8;
    const auto another = cache.Acquire(desc);
    ASSERT_TRUE(another);
    EXPECT_NE(another.Get(), first);
    EXPECT_EQ(another->GetDesc().Groups[0].Layout, first->GetDesc().Groups[0].Layout);
    EXPECT_EQ(device.GroupCreates, 1u);
}
TEST(PipelineLayoutCache, SeparatesStagesNativeLocationsAndPushConstants) {
    LayoutDevice device;
    PipelineLayoutCache cache{device};
    auto desc = ResourceLayout();
    const auto first = cache.Acquire(desc);
    ASSERT_TRUE(first);
    desc.Groups[0].Bindings[0].Binding.Stages = render::ShaderStage::Vertex;
    EXPECT_NE(cache.Acquire(desc).Get(), first.Get());
    desc = ResourceLayout();
    desc.Groups[0].Bindings[0].Binding.D3D12->Space = 1;
    EXPECT_NE(cache.Acquire(desc).Get(), first.Get());
    desc = ResourceLayout();
    desc.PushConstants.push_back({"Push", {.PushId = 0, .Size = 4, .Stages = render::ShaderStage::Vertex, .D3D12 = render::PushConstantDescriptor::D3D12Location{}}, {}});
    const auto push = cache.Acquire(desc);
    ASSERT_TRUE(push);
    EXPECT_NE(push.Get(), first.Get());
    EXPECT_EQ(push->GetDesc().Groups[0].Layout, first->GetDesc().Groups[0].Layout);
    desc.PushConstants[0].Push.Size = 8;
    EXPECT_NE(cache.Acquire(desc).Get(), push.Get());
}
TEST(PipelineLayoutCache, SamplerHashRespectsEqualityAndStateDifferences) {
    LayoutDevice device;
    PipelineLayoutCache cache{device};
    auto desc = ResourceLayout();
    desc.Groups[0].Bindings.resize(1);
    auto& binding = desc.Groups[0].Bindings[0].Binding;
    binding.Kind = render::BindingKind::Sampler;
    binding.D3D12->RegisterType = render::BindingRegisterClass::Sampler;
    binding.ImmutableSampler.emplace();
    const auto first = cache.Acquire(desc);
    ASSERT_TRUE(first);
    binding.ImmutableSampler->MipLodBias = -0.0f;
    EXPECT_EQ(cache.Acquire(desc).Get(), first.Get());
    binding.ImmutableSampler->MipLodBias = 1.0f;
    EXPECT_NE(cache.Acquire(desc).Get(), first.Get());
}
TEST(PipelineLayoutCache, FailedCreationCanBeRetriedAndInvalidDescriptionsCreateNothing) {
    LayoutDevice device;
    PipelineLayoutCache cache{device};
    auto desc = ResourceLayout();
    device.FailGroup = true;
    EXPECT_FALSE(cache.Acquire(desc));
    device.FailGroup = false;
    device.FailLayout = true;
    EXPECT_FALSE(cache.Acquire(desc));
    device.FailLayout = false;
    EXPECT_TRUE(cache.Acquire(desc));
    EXPECT_EQ(device.GroupCreates, 2u);
    EXPECT_EQ(device.LayoutCreates, 2u);
    desc.Groups[0].Bindings[1].Binding.BindingId = 0;
    EXPECT_FALSE(cache.Acquire(desc));
    EXPECT_EQ(device.LayoutCreates, 2u);
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
    void Write(bool duplicate = false, bool missing = false, uint32_t version = 1) {
        auto desc = MakeShaderDesc(MaterialLayout());
        desc.Family.Variants[0].SPIRV = desc.Family.Variants[0].DXIL;
        const auto layout = WriteShaderFamilyLayoutBinary(desc.Family);
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
    AssetManager assets{Device};
    ASSERT_TRUE(assets.LoadShader(Directory / "shader.index.json"));
    EXPECT_EQ(Device.ShaderCreates, 2u);
    EXPECT_EQ(Device.LayoutCreates, 0u);
}
TEST_F(ShaderAssetFiles, RejectsDuplicateMissingStagesAndUnsupportedIndex) {
    AssetManager assets{Device};
    Write(true);
    EXPECT_FALSE(assets.LoadShader(Directory / "shader.index.json"));
    Write(false, true);
    EXPECT_FALSE(assets.LoadShader(Directory / "shader.index.json"));
    Write(false, false, 2);
    EXPECT_FALSE(assets.LoadShader(Directory / "shader.index.json"));
    EXPECT_EQ(Device.ShaderCreates, 0u);
}
}  // namespace
}  // namespace radray
