#include <gtest/gtest.h>

#include "test_asset_fakes.h"
#include "../../../examples/mesh_drawing_pipeline/gpu_support.h"

namespace radray {
namespace {
class PublishDevice final : public test::LayoutDevice {
public:
    Nullable<unique_ptr<gpu::GraphicsPipelineState>> CreateGraphicsPipelineState(const gpu::GraphicsPipelineStateDescriptor&) noexcept override {
        if (++Creates == FailAt) return nullptr;
        return make_unique<test::FakePipeline>();
    }
    unsigned Creates{}, FailAt{};
};

TEST(ShaderPublish, LastPipelineFailurePreservesLiveSetAndSuccessfulRetryRetiresOldSet) {
    auto device = make_shared<PublishDevice>();
    examples::mesh_drawing::GpuResources resources;
    resources.Device = make_unique<DeviceContext>(device, &device->Queue);
    resources.LayoutRegistry = make_unique<PipelineLayoutRegistry>(*device);
    resources.Assets = make_unique<AssetManager>();
    auto desc = test::MakeShaderDesc(test::ResourceLayout());
    resources.Shader = test::CreateTestShader(*resources.Assets, *device, desc);
    ASSERT_TRUE(resources.Shader);
    const auto handle = resources.Shader.GetHandle();
    auto material = CreateMaterial(*resources.Assets, test::MakeMaterialDesc(handle));
    ASSERT_TRUE(material);
    auto second = CreateMaterial(*resources.Assets, test::MakeMaterialDesc(handle));
    ASSERT_TRUE(second);
    const VertexInput input;
    ASSERT_TRUE(resources.CreatePipelines(input, material));
    ASSERT_TRUE(resources.CreatePipelines(input, second));
    Scene scene;
    test::CallbackPipeline* pipeline{};
    auto scheduler = test::CreateScheduler(*resources.Device, pipeline, 2);
    ASSERT_TRUE(scheduler);
    const auto oldStage = resources.Shader.Get()->GetStage(0, gpu::ShaderStage::Vertex)->Target;
    const auto oldColor = resources.Pipelines[0].Color.Get();
    const auto secondColor = resources.Pipelines[1].Color.Get();
    auto candidate = ShaderAsset::Create(*device, desc);
    ASSERT_TRUE(candidate);
    pipeline->OnRecord = [&](FrameSlot& slot) {
        EXPECT_TRUE(slot.AcquireCommandBuffer());
        EXPECT_EQ(resources.Pipelines[0].Color.Get(), oldColor);
        EXPECT_EQ(resources.Shader.Get()->GetStage(0, gpu::ShaderStage::Vertex)->Target, oldStage);
    };
    scheduler->RunFrame();
    device->FailAt = device->Creates + 2;
    EXPECT_FALSE(resources.PublishShader(std::move(*candidate), input, scene, {}, scheduler->BeginFrame()));
    EXPECT_EQ(device->Creates, device->FailAt);
    EXPECT_EQ(resources.Pipelines[1].Color.Get(), secondColor);
    scheduler->RunFrame();
    pipeline->OnRecord = {};
    auto& slot = scheduler->BeginFrame();
    const auto refs = resources.Assets->GetRefCount(handle.Untyped());
    ASSERT_TRUE(resources.PublishShader(std::move(*candidate), input, scene, {}, slot));
    EXPECT_EQ(resources.Shader.GetHandle(), handle);
    EXPECT_EQ(material.Get()->GetPrograms()[0].Shader, handle);
    EXPECT_EQ(resources.Assets->GetRefCount(handle.Untyped()), refs);
    EXPECT_NE(resources.Pipelines[0].Color.Get(), oldColor);
    EXPECT_NE(resources.Shader.Get()->GetStage(0, gpu::ShaderStage::Vertex)->Target, oldStage);
    EXPECT_EQ(device->ShaderDestroys, 0u);
    scheduler->RunFrame();
    EXPECT_EQ(device->ShaderDestroys, 0u);
    scheduler->WaitIdle();
    EXPECT_EQ(device->ShaderDestroys, 2u);
}
}  // namespace
}  // namespace radray
