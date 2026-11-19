#include "test_asset_fakes.h"
#include <gtest/gtest.h>

#include <radray/runtime/gpu_pipeline_layout.h>
#include <radray/runtime/pipeline_layout_registry.h>
#include <radray/runtime/pso_cache.h>

namespace radray::test {
namespace {

class ObservedPso final : public gpu::GraphicsPipelineState {
public:
    ObservedPso(weak_ptr<GpuPipelineLayout> layout, uint32_t& destroyed) : Layout(std::move(layout)), Destroyed(destroyed) {}
    ~ObservedPso() noexcept override {
        EXPECT_FALSE(Layout.expired());
        ++Destroyed;
    }
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}
    Nullable<gpu::PipelineLayout*> GetLayout() const noexcept override {
        const auto locked = Layout.lock();
        return locked ? &locked->GetLayout() : nullptr;
    }
    weak_ptr<GpuPipelineLayout> Layout;
    uint32_t& Destroyed;
};

class PsoDevice final : public LayoutDevice {
public:
    class Compute final : public gpu::ComputePipelineState {
    public:
        bool IsValid() const noexcept override { return true; }
        void Destroy() noexcept override {}
        void SetDebugName(std::string_view) noexcept override {}
        Nullable<gpu::PipelineLayout*> GetLayout() const noexcept override { return nullptr; }
    };
    Nullable<unique_ptr<gpu::ComputePipelineState>> CreateComputePipelineState(const gpu::ComputePipelineStateDescriptor& desc) noexcept override {
        LastShader = desc.CS.Target;
        return make_unique<Compute>();
    }
    Nullable<unique_ptr<gpu::GraphicsPipelineState>> CreateGraphicsPipelineState(const gpu::GraphicsPipelineStateDescriptor& desc) noexcept override {
        LastShader = desc.VS->Target;
        EXPECT_EQ(desc.PipelineLayout, &ExpectedLayout.lock()->GetLayout());
        return make_unique<ObservedPso>(ExpectedLayout, Destroyed);
    }
    weak_ptr<GpuPipelineLayout> ExpectedLayout;
    gpu::Shader* LastShader{};
    uint32_t Destroyed{};
};

TEST(PsoCacheTest, ExplicitLayoutAndPsoRetireTogetherAfterCompletion) {
    auto device = make_shared<PsoDevice>();
    DeviceContext ctx{device, &device->Queue};
    AssetManager assets;
    PipelineLayoutRegistry registry{*device};
    PsoCache cache{*device, assets, registry};
    auto shader = CreateTestShader(assets, *device, MakeShaderDesc());
    auto layout = registry.Acquire(MaterialLayout()).Unwrap();
    device->ExpectedLayout = layout;
    GraphicsPsoKey key{.Shader = shader.GetHandle(), .ExplicitLayout = layout};
    auto first = cache.AcquireGraphics(key);
    ASSERT_TRUE(first);
    EXPECT_EQ(first.Get(), cache.AcquireGraphics(key).Get());
    CallbackPipeline* callbacks{};
    auto scheduler = CreateScheduler(ctx, callbacks, 2);
    scheduler->RunFrame();
    auto& slot = scheduler->BeginFrame();
    auto replacement = ShaderAsset::Create(*device, MakeShaderDesc());
    AssetTransaction tx{assets};
    ASSERT_TRUE(tx.StageReplace(shader, std::move(*replacement)));
    ASSERT_TRUE(assets.Commit(std::move(tx), *slot.Emplace<AssetGarbageBatch>()).Succeeded);
    auto previous = device->ExpectedLayout;
    key.ExplicitLayout.reset();
    layout.reset();
    cache.RetireInvalidated(slot);
    EXPECT_FALSE(previous.expired());
    EXPECT_EQ(device->Destroyed, 0u);
    scheduler->RunFrame();
    scheduler->WaitIdle();
    EXPECT_TRUE(previous.expired());
    EXPECT_EQ(device->Destroyed, 1u);
}

TEST(PsoCacheTest, EqualHandleNumbersResolveAgainstEachCachesAssetManager) {
    PsoDevice device;
    PipelineLayoutRegistry registry{device};
    AssetManager first, second;
    auto shader1 = CreateTestShader(first, device, MakeShaderDesc());
    auto shader2 = CreateTestShader(second, device, MakeShaderDesc());
    ASSERT_EQ(shader1.GetHandle(), shader2.GetHandle());
    auto layout = registry.Acquire(shader1.Get()->GetLayoutDesc()).Unwrap();
    device.ExpectedLayout = layout;
    PsoCache a{device, first, registry}, b{device, second, registry};
    GraphicsPsoKey key{.Shader = shader1.GetHandle()};
    ASSERT_TRUE(a.AcquireGraphics(key));
    EXPECT_EQ(device.LastShader, shader1.Get()->GetStage(0, gpu::ShaderStage::Vertex)->Target);
    ASSERT_TRUE(b.AcquireGraphics(key));
    EXPECT_EQ(device.LastShader, shader2.Get()->GetStage(0, gpu::ShaderStage::Vertex)->Target);
    EXPECT_FALSE(a.AcquireCompute({.Shader = shader1.GetHandle()}));
}

TEST(PsoCacheTest, ComputeVariantsCacheIndependentlyOfExplicitLayout) {
    PsoDevice device;
    PipelineLayoutRegistry registry{device};
    AssetManager assets;
    auto desc = MakeShaderDesc();
    desc.Family.Variants[0].DXIL->Stages = gpu::ShaderStage::Compute;
    desc.Stages = {{0, gpu::GpuBackend::D3D12, gpu::ShaderStage::Compute, "CS", {byte{3}}}};
    auto shader = CreateTestShader(assets, device, desc);
    ASSERT_TRUE(shader);
    PsoCache cache{device, assets, registry};
    ComputePsoKey key{.Shader = shader.GetHandle()};
    auto inferred = cache.AcquireCompute(key);
    ASSERT_TRUE(inferred);
    EXPECT_EQ(inferred.Get(), cache.AcquireCompute(key).Get());
    EXPECT_EQ(device.LastShader, shader.Get()->GetStage(0, gpu::ShaderStage::Compute)->Target);
    key.ExplicitLayout = registry.Acquire(MaterialLayout()).Unwrap();
    auto explicitState = cache.AcquireCompute(key);
    ASSERT_TRUE(explicitState);
    EXPECT_NE(explicitState.Get(), inferred.Get());
    key.Variant = 2;
    EXPECT_FALSE(cache.AcquireCompute(key));
}

}  // namespace
}  // namespace radray::test
