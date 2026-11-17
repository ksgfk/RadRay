#include <gtest/gtest.h>
#include <algorithm>
#include <radray/runtime/pipeline_layout_registry.h>
#include "test_asset_fakes.h"

namespace radray {
namespace {
using namespace test;

TEST(PipelineLayoutRegistry, OwnsKeysAndReusesLayoutsAcrossFamiliesAndOrdering) {
    LayoutDevice device;
    PipelineLayoutRegistry registry{device};
    auto first = registry.Acquire(ResourceLayout());
    ASSERT_TRUE(first);
    auto desc = ResourceLayout();
    desc.Groups[0].Name = "Renamed";
    desc.Groups[0].Bindings[0].Name = "Other";
    desc.Groups[0].Bindings[0].Binding.Vulkan->Binding = 99;
    std::reverse(desc.Groups[0].Bindings.begin(), desc.Groups[0].Bindings.end());
    EXPECT_EQ(registry.Acquire(desc).Get(), first.Get());
    EXPECT_EQ(device.GroupCreates, 1u);
    EXPECT_EQ(device.LayoutCreates, 1u);
    desc.Groups[0].GroupSlot = 8;
    const auto another = registry.Acquire(desc);
    ASSERT_TRUE(another);
    EXPECT_NE(another.Get(), first.Get());
    EXPECT_EQ(another->GetLayout().GetDesc().Groups[0].Layout, first->GetLayout().GetDesc().Groups[0].Layout);
    EXPECT_EQ(device.GroupCreates, 1u);
}
TEST(PipelineLayoutRegistry, OwnersOutliveRegistryAndReleaseGroupsIndependently) {
    LayoutDevice device;
    Nullable<shared_ptr<GpuPipelineLayout>> first, second;
    Nullable<shared_ptr<gpu::BindingGroupLayout>> group;
    {
        PipelineLayoutRegistry registry{device};
        auto desc = ResourceLayout();
        first = registry.Acquire(desc);
        ASSERT_TRUE(first);
        group = first->GetGroupLayout(0);
        ASSERT_TRUE(group);
        EXPECT_FALSE(first->GetGroupLayout(1));
        desc.Groups[0].GroupSlot = 7;
        second = registry.Acquire(desc);
        ASSERT_TRUE(second);
        EXPECT_EQ(second->GetGroupLayout(7).Get(), group.Get());
    }
    EXPECT_EQ(device.LayoutDestroys, 0u);
    first = nullptr;
    EXPECT_EQ(device.LayoutDestroys, 1u);
    second = nullptr;
    EXPECT_EQ(device.LayoutDestroys, 2u);
    EXPECT_EQ(device.GroupDestroys, 0u);
    group = nullptr;
    EXPECT_EQ(device.GroupDestroys, 1u);
}

TEST(PipelineLayoutRegistry, RecreatesExpiredLayoutsAcrossSweeps) {
    LayoutDevice device;
    PipelineLayoutRegistry registry{device};
    auto desc = ResourceLayout();
    auto pinned = registry.Acquire(desc);
    ASSERT_TRUE(pinned);
    for (uint32_t space = 1; space <= 256; ++space) {
        desc.Groups[0].Bindings[0].Binding.D3D12->Space = space;
        ASSERT_TRUE(registry.Acquire(desc));
        EXPECT_EQ(device.LayoutDestroys, space);
        EXPECT_EQ(device.GroupDestroys, space);
    }
    desc = ResourceLayout();
    EXPECT_EQ(registry.Acquire(desc).Get(), pinned.Get());
    pinned = nullptr;
    EXPECT_EQ(device.LayoutDestroys, 257u);
    EXPECT_EQ(device.GroupDestroys, 257u);
    ASSERT_TRUE(registry.Acquire(desc));
    EXPECT_EQ(device.LayoutCreates, 258u);
    EXPECT_EQ(device.GroupCreates, 258u);
}

TEST(PipelineLayoutRegistry, SeparatesStagesNativeLocationsAndPushConstants) {
    LayoutDevice device;
    PipelineLayoutRegistry registry{device};
    auto desc = ResourceLayout();
    const auto first = registry.Acquire(desc);
    ASSERT_TRUE(first);
    desc.Groups[0].Bindings[0].Binding.Stages = gpu::ShaderStage::Vertex;
    EXPECT_NE(registry.Acquire(desc).Get(), first.Get());
    desc = ResourceLayout();
    desc.Groups[0].Bindings[0].Binding.D3D12->Space = 1;
    EXPECT_NE(registry.Acquire(desc).Get(), first.Get());
    desc = ResourceLayout();
    desc.PushConstants.push_back({"Push", {.PushId = 0, .Size = 4, .Stages = gpu::ShaderStage::Vertex, .D3D12 = gpu::PushConstantDescriptor::D3D12Location{}}, {}});
    const auto push = registry.Acquire(desc);
    ASSERT_TRUE(push);
    EXPECT_NE(push.Get(), first.Get());
    EXPECT_EQ(push->GetLayout().GetDesc().Groups[0].Layout, first->GetLayout().GetDesc().Groups[0].Layout);
    desc.PushConstants[0].Push.Size = 8;
    EXPECT_NE(registry.Acquire(desc).Get(), push.Get());
}
TEST(PipelineLayoutRegistry, SamplerHashRespectsEqualityAndStateDifferences) {
    LayoutDevice device;
    PipelineLayoutRegistry registry{device};
    auto desc = ResourceLayout();
    desc.Groups[0].Bindings.resize(1);
    auto& binding = desc.Groups[0].Bindings[0].Binding;
    binding.Kind = gpu::BindingKind::Sampler;
    binding.D3D12->RegisterType = gpu::BindingRegisterClass::Sampler;
    binding.ImmutableSampler.emplace();
    const auto first = registry.Acquire(desc);
    ASSERT_TRUE(first);
    binding.ImmutableSampler->MipLodBias = -0.0f;
    EXPECT_EQ(registry.Acquire(desc).Get(), first.Get());
    binding.ImmutableSampler->MipLodBias = 1.0f;
    EXPECT_NE(registry.Acquire(desc).Get(), first.Get());
}
TEST(PipelineLayoutRegistry, FailedCreationCanBeRetriedAndInvalidDescriptionsCreateNothing) {
    LayoutDevice device;
    PipelineLayoutRegistry registry{device};
    auto desc = ResourceLayout();
    device.FailGroup = true;
    EXPECT_FALSE(registry.Acquire(desc));
    device.FailGroup = false;
    device.FailLayout = true;
    EXPECT_FALSE(registry.Acquire(desc));
    EXPECT_EQ(device.GroupDestroys, 1u);
    device.FailLayout = false;
    EXPECT_TRUE(registry.Acquire(desc));
    EXPECT_EQ(device.GroupCreates, 3u);
    EXPECT_EQ(device.LayoutCreates, 2u);
    desc.Groups[0].Bindings[1].Binding.BindingId = 0;
    EXPECT_FALSE(registry.Acquire(desc));
    EXPECT_EQ(device.LayoutCreates, 2u);
}

}  // namespace
}  // namespace radray
