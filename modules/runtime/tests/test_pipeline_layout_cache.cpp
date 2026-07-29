// PipelineLayoutCache: 按 binding 布局内容去重 PipelineLayout, 引用计数共享。
//
// 【覆盖重点】是三件事:
//   1. 去重真的按【内容】而非指针 —— 含 manifest 书写顺序不同但语义相同必须命中;
//   2. 引用计数的两端 —— 多个持有者共享一个对象, 归零时对象自己从索引摘除;
//   3. 缓存先死于持有者不崩 —— 这是 Application 关停的常规路径 (RenderSystem 先于
//      AssetManager 销毁), 不是异常路径。
//
// 需要真实 device: layout 是 GPU 对象 (D3D12 的 ID3D12RootSignature / Vulkan 的
// VkPipelineLayout), 没有可替换的假实现。无设备时 GTEST_SKIP。

#include <radray/runtime/pipeline_layout_cache.h>

#include <radray/render/rhi.h>
#include <radray/types.h>

#include <gtest/gtest.h>

#include <array>
#include <optional>

namespace radray {
namespace {

/// 一个 device。本文件只建 PipelineLayout, 不提交命令, 故不要队列。
struct DeviceContext {
    bool VulkanEnvInitialized{false};
    unique_ptr<render::DXGIFactory> Factory;
    shared_ptr<render::Device> Device;

    ~DeviceContext() {
        Device.reset();
        Factory.reset();
#if defined(RADRAY_ENABLE_VULKAN)
        if (VulkanEnvInitialized) {
            render::InstanceVulkan::ShutdownEnv();
        }
#endif
    }
};

bool TryCreateAnyDevice(DeviceContext& ctx) {
#if defined(RADRAY_ENABLE_D3D12)
    {
        render::DXGIFactoryDescriptor factoryDesc{};
        factoryDesc.IsEnableDebugLayer = false;
        auto factory = render::DXGIFactory::Create(factoryDesc);
        if (factory.HasValue()) {
            ctx.Factory = factory.Release();
            render::D3D12DeviceDescriptor d3d12Desc{};
            d3d12Desc.Factory = ctx.Factory.get();
            auto device = render::Device::Create(render::DeviceDescriptor{d3d12Desc});
            if (device.HasValue()) {
                ctx.Device = device.Release();
                return true;
            }
            ctx.Factory.reset();
        }
    }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    {
        render::VulkanInstanceDescriptor instanceDesc{};
        instanceDesc.AppName = "radray_pipeline_layout_cache_test";
        instanceDesc.EngineName = "radray";
        instanceDesc.IsEnableDebugLayer = false;
        if (render::InstanceVulkan::InitEnv(instanceDesc)) {
            ctx.VulkanEnvInitialized = true;
            render::VulkanDeviceDescriptor vkDesc{};
            auto device = render::Device::Create(render::DeviceDescriptor{vkDesc});
            if (device.HasValue()) {
                ctx.Device = device.Release();
                return true;
            }
        }
    }
#endif
    return false;
}

using Entry = render::ShaderParameterSetLayoutEntryDescriptor;

Entry MakeEntry(
    uint32_t binding,
    render::ShaderParameterBindingType type,
    render::ShaderStages stages) noexcept {
    Entry entry{};
    entry.Binding = binding;
    entry.Type = type;
    entry.Count = 1;
    entry.Stages = stages;
    return entry;
}

/// PipelineLayoutDescriptor 内是 span, 必须有稳定拥有者。测试里就地拼一份。
class DescriptorBuilder {
public:
    /// 组按给定顺序追加, entry 也按给定顺序 —— 顺序无关性正是要验证的东西之一。
    DescriptorBuilder& AddGroup(uint32_t groupIndex, vector<Entry> entries) {
        _entries.push_back(make_unique<vector<Entry>>(std::move(entries)));
        render::ShaderParameterSetLayoutDescriptor set{};
        set.GroupIndex = groupIndex;
        set.Entries = *_entries.back();
        _sets.push_back(set);
        return *this;
    }

    DescriptorBuilder& SetPushConstant(render::PushConstantDescriptor pushConstant) {
        _pushConstant = pushConstant;
        return *this;
    }

    render::PipelineLayoutDescriptor Get() const noexcept {
        render::PipelineLayoutDescriptor desc{};
        desc.ParameterSets = _sets;
        desc.PushConstant = _pushConstant;
        return desc;
    }

private:
    vector<unique_ptr<vector<Entry>>> _entries;
    vector<render::ShaderParameterSetLayoutDescriptor> _sets;
    std::optional<render::PushConstantDescriptor> _pushConstant;
};

/// error_pass 的形状: group 0 一个 VS CBuffer, group 1 一个 VS CBuffer。
DescriptorBuilder MakeTwoGroupDescriptor() {
    DescriptorBuilder builder;
    builder
        .AddGroup(0, {MakeEntry(1, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)})
        .AddGroup(1, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    return builder;
}

class PipelineLayoutCacheTest : public testing::Test {
protected:
    void SetUp() override {
        if (!TryCreateAnyDevice(_ctx)) {
            GTEST_SKIP() << "no render backend is available on this machine";
        }
        _cache = make_unique<PipelineLayoutCache>(_ctx.Device.get());
    }

    void TearDown() override { _cache.reset(); }

    PipelineLayoutCache& Cache() { return *_cache; }
    render::Device& Device() { return *_ctx.Device; }

private:
    DeviceContext _ctx;
    unique_ptr<PipelineLayoutCache> _cache;
};

}  // namespace

// ============================ key 的归一化 ============================

TEST(PipelineLayoutCacheKeyTest, NormalizesGroupAndBindingOrder) {
    DescriptorBuilder ordered;
    ordered
        .AddGroup(0, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex),
                      MakeEntry(1, render::ShaderParameterBindingType::Texture, render::ShaderStage::Pixel)})
        .AddGroup(1, {MakeEntry(0, render::ShaderParameterBindingType::Sampler, render::ShaderStage::Pixel)});

    // 组顺序与 binding 顺序都反过来写, 语义完全相同。
    DescriptorBuilder shuffled;
    shuffled
        .AddGroup(1, {MakeEntry(0, render::ShaderParameterBindingType::Sampler, render::ShaderStage::Pixel)})
        .AddGroup(0, {MakeEntry(1, render::ShaderParameterBindingType::Texture, render::ShaderStage::Pixel),
                      MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});

    const PipelineLayoutCacheKey lhs = PipelineLayoutCacheKey::Build(ordered.Get());
    const PipelineLayoutCacheKey rhs = PipelineLayoutCacheKey::Build(shuffled.Get());
    EXPECT_TRUE(lhs == rhs) << "the manifest writing order must not split cache entries";

    // 归一化后的形状本身也要对 —— 否则"相等"可能只是两边一样错。
    ASSERT_EQ(lhs.Groups.size(), 2u);
    EXPECT_EQ(lhs.Groups[0].GroupIndex, 0u);
    EXPECT_EQ(lhs.Groups[1].GroupIndex, 1u);
    ASSERT_EQ(lhs.Groups[0].Entries.size(), 2u);
    EXPECT_EQ(lhs.Groups[0].Entries[0].Binding, 0u);
    EXPECT_EQ(lhs.Groups[0].Entries[1].Binding, 1u);
}

TEST(PipelineLayoutCacheKeyTest, DistinguishesRealDifferences) {
    const PipelineLayoutCacheKey base = PipelineLayoutCacheKey::Build(MakeTwoGroupDescriptor().Get());

    // binding 号不同。
    DescriptorBuilder otherBinding;
    otherBinding
        .AddGroup(0, {MakeEntry(2, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)})
        .AddGroup(1, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    EXPECT_FALSE(base == PipelineLayoutCacheKey::Build(otherBinding.Get()));

    // stage 不同 —— 它进 root signature / VkDescriptorSetLayoutBinding, 必须分条。
    DescriptorBuilder otherStage;
    otherStage
        .AddGroup(0, {MakeEntry(1, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Pixel)})
        .AddGroup(1, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    EXPECT_FALSE(base == PipelineLayoutCacheKey::Build(otherStage.Get()));

    // 组少一个。
    DescriptorBuilder oneGroup;
    oneGroup.AddGroup(0, {MakeEntry(1, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    EXPECT_FALSE(base == PipelineLayoutCacheKey::Build(oneGroup.Get()));

    // push constant 有无。
    DescriptorBuilder withPushConstant = MakeTwoGroupDescriptor();
    withPushConstant.SetPushConstant(render::PushConstantDescriptor{
        .Location = {.Group = 2, .Binding = 0},
        .Size = 16,
        .Stages = render::ShaderStage::Vertex});
    EXPECT_FALSE(base == PipelineLayoutCacheKey::Build(withPushConstant.Get()));
}

/// key 被拷贝 / 移动后, Get() 里的 span 必须重新指向新位置。
TEST(PipelineLayoutCacheKeyTest, DescriptorViewSurvivesCopyAndMove) {
    const PipelineLayoutCacheKey original =
        PipelineLayoutCacheKey::Build(MakeTwoGroupDescriptor().Get());

    // Get() 无条件重建视图, 故拷贝 / 移动后无需任何额外调用。
    PipelineLayoutCacheKey copy = original;
    const render::PipelineLayoutDescriptor copied = copy.Get();
    ASSERT_EQ(copied.ParameterSets.size(), 2u);
    ASSERT_EQ(copied.ParameterSets[0].Entries.size(), 1u);
    EXPECT_EQ(copied.ParameterSets[0].Entries.data(), copy.Groups[0].Entries.data());

    PipelineLayoutCacheKey moved = std::move(copy);
    const render::PipelineLayoutDescriptor movedDesc = moved.Get();
    ASSERT_EQ(movedDesc.ParameterSets.size(), 2u);
    EXPECT_EQ(movedDesc.ParameterSets[1].Entries.data(), moved.Groups[1].Entries.data());
}

// ============================ 去重与引用计数 ============================

TEST_F(PipelineLayoutCacheTest, SameContentSharesOneLayout) {
    const DescriptorBuilder builder = MakeTwoGroupDescriptor();
    SharedPipelineLayoutRef first = Cache().GetOrCreate(builder.Get());
    ASSERT_TRUE(first.HasValue());
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    EXPECT_EQ(Cache().GetMissCount(), 1u);
    EXPECT_EQ(Cache().GetHitCount(), 0u);

    // 另一份独立拼出的 descriptor —— 内容相同, 但没有任何指针关系。
    const DescriptorBuilder identical = MakeTwoGroupDescriptor();
    SharedPipelineLayoutRef second = Cache().GetOrCreate(identical.Get());
    ASSERT_TRUE(second.HasValue());
    EXPECT_EQ(second.Get(), first.Get()) << "identical content must share one layout object";
    EXPECT_EQ(second->Get(), first->Get());
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    EXPECT_EQ(Cache().GetHitCount(), 1u);
    EXPECT_EQ(first->GetRefCount(), 2u);
}

/// 书写顺序不同必须命中同一个【GPU 对象】, 不只是 key 相等。
TEST_F(PipelineLayoutCacheTest, WritingOrderDoesNotSplitLayouts) {
    DescriptorBuilder ordered;
    ordered.AddGroup(
        0,
        {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex),
         MakeEntry(1, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Pixel)});

    DescriptorBuilder shuffled;
    shuffled.AddGroup(
        0,
        {MakeEntry(1, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Pixel),
         MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});

    SharedPipelineLayoutRef first = Cache().GetOrCreate(ordered.Get());
    ASSERT_TRUE(first.HasValue());
    SharedPipelineLayoutRef second = Cache().GetOrCreate(shuffled.Get());
    ASSERT_TRUE(second.HasValue());
    EXPECT_EQ(second.Get(), first.Get());
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
}

TEST_F(PipelineLayoutCacheTest, DifferentContentSplitsLayouts) {
    SharedPipelineLayoutRef first = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
    ASSERT_TRUE(first.HasValue());

    DescriptorBuilder other;
    other.AddGroup(
        0,
        {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    SharedPipelineLayoutRef second = Cache().GetOrCreate(other.Get());
    ASSERT_TRUE(second.HasValue());

    EXPECT_NE(second.Get(), first.Get());
    EXPECT_NE(second->Get(), first->Get());
    EXPECT_EQ(Cache().GetLayoutCount(), 2u);
    EXPECT_EQ(Cache().GetMissCount(), 2u);
    EXPECT_EQ(Cache().GetHitCount(), 0u);
}

TEST_F(PipelineLayoutCacheTest, LastReleaseDetachesFromCache) {
    {
        SharedPipelineLayoutRef first = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
        ASSERT_TRUE(first.HasValue());
        {
            SharedPipelineLayoutRef second = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
            ASSERT_EQ(second.Get(), first.Get());
            EXPECT_EQ(first->GetRefCount(), 2u);
            EXPECT_EQ(Cache().GetLayoutCount(), 1u);
        }
        // 还有一个持有者, 不该销毁。
        EXPECT_EQ(first->GetRefCount(), 1u);
        EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    }
    EXPECT_EQ(Cache().GetLayoutCount(), 0u)
        << "the entry must be removed the moment the last reference goes away";
}

/// 归零后同样的内容再取一次, 应当是一次全新的 miss —— 证明上一轮真的销毁了, 而不是
/// 留了个墓碑等着被 lock() 发现。
TEST_F(PipelineLayoutCacheTest, RecreatesAfterFullRelease) {
    {
        SharedPipelineLayoutRef first = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
        ASSERT_TRUE(first.HasValue());
    }
    ASSERT_EQ(Cache().GetLayoutCount(), 0u);

    SharedPipelineLayoutRef again = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
    ASSERT_TRUE(again.HasValue());
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    EXPECT_EQ(Cache().GetMissCount(), 2u);
    EXPECT_EQ(Cache().GetHitCount(), 0u);
}

/// 热更新的形状: 布局没变时 reload 必须命中同一个 GPU 对象, 不重建 root signature。
TEST_F(PipelineLayoutCacheTest, ReloadWithUnchangedLayoutDoesNotRecreate) {
    SharedPipelineLayoutRef oldRef = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
    ASSERT_TRUE(oldRef.HasValue());
    render::PipelineLayout* nativeBefore = oldRef->Get();
    const uint64_t missesBefore = Cache().GetMissCount();

    // 新 program 先取引用, 旧的再放开 —— reload 的真实顺序。
    SharedPipelineLayoutRef newRef = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
    ASSERT_TRUE(newRef.HasValue());
    oldRef.Reset();

    EXPECT_EQ(newRef->Get(), nativeBefore) << "an unchanged layout must not be recreated";
    EXPECT_EQ(Cache().GetMissCount(), missesBefore);
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    EXPECT_EQ(newRef->GetRefCount(), 1u);
}

TEST_F(PipelineLayoutCacheTest, AcceptsEmptyDescriptor) {
    // 无组无 push constant 的 layout 在两个后端都是合法的 (空 root signature)。
    const DescriptorBuilder empty;
    SharedPipelineLayoutRef ref = Cache().GetOrCreate(empty.Get());
    ASSERT_TRUE(ref.HasValue()) << "an empty layout is legal in both backends";
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
}

// ============================ 关停顺序 ============================

/// 【这是 Application 关停的常规路径, 不是异常路径】: RenderSystem (缓存的宿主) 先于
/// AssetManager 销毁, 而资产要到 AssetManager 析构时才放开最后一份 layout 引用。
TEST(PipelineLayoutCacheShutdownTest, CacheMayDieBeforeItsHolders) {
    DeviceContext ctx;
    if (!TryCreateAnyDevice(ctx)) {
        GTEST_SKIP() << "no render backend is available on this machine";
    }

    SharedPipelineLayoutRef survivor;
    {
        PipelineLayoutCache cache{ctx.Device.get()};
        survivor = cache.GetOrCreate(MakeTwoGroupDescriptor().Get());
        ASSERT_TRUE(survivor.HasValue());
        EXPECT_EQ(cache.GetLayoutCount(), 1u);
    }
    // 缓存已死, layout 仍然可用 —— 引用计数保住了它。
    ASSERT_TRUE(survivor.HasValue());
    EXPECT_NE(survivor->Get(), nullptr);
    EXPECT_EQ(survivor->GetRefCount(), 1u);

    // 之后归零走的是 _cache == nullptr 那条分支, 直接销毁, 不回调已死的缓存。
    survivor.Reset();
    EXPECT_FALSE(survivor.HasValue());
}

TEST(PipelineLayoutCacheShutdownTest, EmptyCacheDiesCleanly) {
    DeviceContext ctx;
    if (!TryCreateAnyDevice(ctx)) {
        GTEST_SKIP() << "no render backend is available on this machine";
    }
    PipelineLayoutCache cache{ctx.Device.get()};
    EXPECT_EQ(cache.GetLayoutCount(), 0u);
    EXPECT_EQ(cache.GetDevice().Get(), ctx.Device.get());
}

}  // namespace radray
