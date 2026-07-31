// PipelineLayoutCache: 按 binding 布局内容去重 PipelineLayout, 引用计数共享。
//
// 【覆盖重点】是四件事:
//   1. 去重真的按【内容】而非指针 —— 含 manifest 书写顺序不同、同组被拆成多个
//      parameter set 但语义相同的情形, 都必须命中;
//   2. key 满足 unordered_map 的契约 —— 内容相等必然散列相等, 含被移动走的源;
//   3. 引用计数的两端 —— 多个持有者共享一个对象, 归零时对象自己从表里摘除;
//   4. 缓存先死于持有者不崩 —— 这是 Application 关停的常规路径 (RenderSystem 先于
//      AssetManager 销毁), 不是异常路径。
//
// 1 与 2 是纯 CPU 数据 (PipelineLayoutKey), 无条件运行。3 与 4 需要真实 device: layout
// 是 GPU 对象 (D3D12 的 ID3D12RootSignature / Vulkan 的 VkPipelineLayout), 没有可替换的
// 假实现, 无设备时 GTEST_SKIP。

#include <radray/runtime/pipeline_layout_cache.h>

#include <radray/render/rhi.h>
#include <radray/types.h>

#include <gtest/gtest.h>

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
    /// 同一个 groupIndex 可以出现多次, 那也是要验证的 (后端会把它们合并)。
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

PipelineLayoutKey BuildKey(const DescriptorBuilder& builder) {
    return PipelineLayoutKey::Build(builder.Get());
}

/// 相等的两个 key 必须散列相等 —— 这是 unordered_map 的硬性契约, 违反即静默查不到。
void ExpectSameKey(const PipelineLayoutKey& lhs, const PipelineLayoutKey& rhs) {
    EXPECT_TRUE(lhs == rhs);
    EXPECT_EQ(lhs.GetHash(), rhs.GetHash()) << "equal keys must hash equal";
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

private:
    DeviceContext _ctx;
    unique_ptr<PipelineLayoutCache> _cache;
};

}  // namespace

// ============================ key 的归一化 ============================

TEST(PipelineLayoutKeyTest, NormalizesGroupAndBindingOrder) {
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

    const PipelineLayoutKey lhs = BuildKey(ordered);
    ExpectSameKey(lhs, BuildKey(shuffled));

    // 归一化后的形状本身也要对 —— 否则"相等"可能只是两边一样错。
    ASSERT_EQ(lhs.GetGroups().size(), 2u);
    EXPECT_EQ(lhs.GetGroups()[0].GroupIndex, 0u);
    EXPECT_EQ(lhs.GetGroups()[1].GroupIndex, 1u);
    const std::span<const Entry> group0 = lhs.GetEntries(lhs.GetGroups()[0]);
    ASSERT_EQ(group0.size(), 2u);
    EXPECT_EQ(group0[0].Binding, 0u);
    EXPECT_EQ(group0[1].Binding, 1u);
    EXPECT_EQ(lhs.GetEntries(lhs.GetGroups()[1]).size(), 1u);
}

/// 【两个后端都把 GroupIndex 相同的 parameter set 拼在一起】(d3d12 的
/// CreateRootSignatureInternal / vk 的 CreatePipelineLayoutInternal), 故"拆成两个 set"与
/// "写成一个 set"会建出逐字节相同的 GPU 对象。不合并就会为它们各建一份 root signature。
TEST(PipelineLayoutKeyTest, MergesParameterSetsSharingAGroupIndex) {
    DescriptorBuilder split;
    split
        .AddGroup(0, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)})
        .AddGroup(0, {MakeEntry(1, render::ShaderParameterBindingType::Texture, render::ShaderStage::Pixel)});

    DescriptorBuilder merged;
    merged.AddGroup(
        0,
        {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex),
         MakeEntry(1, render::ShaderParameterBindingType::Texture, render::ShaderStage::Pixel)});

    const PipelineLayoutKey key = BuildKey(split);
    ExpectSameKey(key, BuildKey(merged));
    ASSERT_EQ(key.GetGroups().size(), 1u) << "the two sets must collapse into one group";
    EXPECT_EQ(key.GetEntries(key.GetGroups()[0]).size(), 2u);
}

/// 空组是【可观测的】, 不能当作"没有这一组": d3d12 会为它留一个 CreateShaderParameterSet
/// 能查到的 parameter group, vk 的 setLayoutCount 取 max(GroupIndex)+1。
TEST(PipelineLayoutKeyTest, KeepsEmptyGroups) {
    DescriptorBuilder withEmpty;
    withEmpty
        .AddGroup(0, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)})
        .AddGroup(1, {});

    DescriptorBuilder withoutEmpty;
    withoutEmpty.AddGroup(
        0,
        {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});

    const PipelineLayoutKey key = BuildKey(withEmpty);
    ASSERT_EQ(key.GetGroups().size(), 2u);
    EXPECT_EQ(key.GetGroups()[1].GroupIndex, 1u);
    EXPECT_TRUE(key.GetEntries(key.GetGroups()[1]).empty());
    EXPECT_FALSE(key == BuildKey(withoutEmpty)) << "an empty group changes the backend layout";
}

TEST(PipelineLayoutKeyTest, DistinguishesRealDifferences) {
    const PipelineLayoutKey base = BuildKey(MakeTwoGroupDescriptor());

    // binding 号不同。
    DescriptorBuilder otherBinding;
    otherBinding
        .AddGroup(0, {MakeEntry(2, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)})
        .AddGroup(1, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    EXPECT_FALSE(base == BuildKey(otherBinding));

    // stage 不同 —— 它进 root signature / VkDescriptorSetLayoutBinding, 必须分条。
    DescriptorBuilder otherStage;
    otherStage
        .AddGroup(0, {MakeEntry(1, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Pixel)})
        .AddGroup(1, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    EXPECT_FALSE(base == BuildKey(otherStage));

    // 组少一个。
    DescriptorBuilder oneGroup;
    oneGroup.AddGroup(0, {MakeEntry(1, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    EXPECT_FALSE(base == BuildKey(oneGroup));

    // 【组号本身不同, 而非只是组数】: 若归一化只比较"第 i 组的内容", 这一对会被误判相等。
    DescriptorBuilder shiftedGroups;
    shiftedGroups
        .AddGroup(0, {MakeEntry(1, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)})
        .AddGroup(2, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    EXPECT_FALSE(base == BuildKey(shiftedGroups));

    // 【entry 落在哪一组不同, 但扁平化后的序列相同】: 这一对能抓到"只比较扁平 entry
    // 数组、忘了比较组的切分"的实现。
    DescriptorBuilder regrouped;
    regrouped.AddGroup(
        0,
        {MakeEntry(1, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex),
         MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    EXPECT_FALSE(base == BuildKey(regrouped));

    // push constant 有无。
    DescriptorBuilder withPushConstant = MakeTwoGroupDescriptor();
    withPushConstant.SetPushConstant(render::PushConstantDescriptor{
        .Location = {.Group = 2, .Binding = 0},
        .Size = 16,
        .Stages = render::ShaderStage::Vertex});
    EXPECT_FALSE(base == BuildKey(withPushConstant));

    // push constant 的字段不同。
    DescriptorBuilder otherPushConstantSize = MakeTwoGroupDescriptor();
    otherPushConstantSize.SetPushConstant(render::PushConstantDescriptor{
        .Location = {.Group = 2, .Binding = 0},
        .Size = 32,
        .Stages = render::ShaderStage::Vertex});
    EXPECT_FALSE(BuildKey(withPushConstant) == BuildKey(otherPushConstantSize));
}

/// ImmutableSampler 是 optional<SamplerDescriptor>, 是 key 里唯一的非平凡成员。逐字段
/// 散列而非 memcmp 的理由就在它身上 —— 填充字节会让相等的对象散列不等。
TEST(PipelineLayoutKeyTest, AccountsForImmutableSampler) {
    Entry plain = MakeEntry(0, render::ShaderParameterBindingType::Sampler, render::ShaderStage::Pixel);

    render::SamplerDescriptor sampler{};
    sampler.AddressS = render::AddressMode::Repeat;
    sampler.MagFilter = render::FilterMode::Linear;
    Entry withSampler = plain;
    withSampler.ImmutableSampler = sampler;

    render::SamplerDescriptor otherSampler = sampler;
    otherSampler.AddressS = render::AddressMode::ClampToEdge;
    Entry withOtherSampler = plain;
    withOtherSampler.ImmutableSampler = otherSampler;

    DescriptorBuilder none;
    none.AddGroup(0, {plain});
    DescriptorBuilder some;
    some.AddGroup(0, {withSampler});
    DescriptorBuilder other;
    other.AddGroup(0, {withOtherSampler});
    DescriptorBuilder same;
    same.AddGroup(0, {withSampler});

    EXPECT_FALSE(BuildKey(none) == BuildKey(some));
    EXPECT_FALSE(BuildKey(some) == BuildKey(other));
    ExpectSameKey(BuildKey(some), BuildKey(same));
}

TEST(PipelineLayoutKeyTest, DefaultKeyEqualsBuiltEmptyKey) {
    const DescriptorBuilder empty;
    // 默认构造的 key 必须与 Build(空 descriptor) 完全一致 —— 否则默认构造出来的 key
    // 会是个查不到任何东西、也散列不对的幽灵。
    ExpectSameKey(PipelineLayoutKey{}, BuildKey(empty));
    EXPECT_TRUE(PipelineLayoutKey{}.GetGroups().empty());
    EXPECT_FALSE(PipelineLayoutKey{}.GetPushConstant().has_value());
}

/// 【被移动走的源必须仍满足散列契约】: 默认的 move 会留下"空容器 + 原散列值"的源, 那个源
/// 与一个真正的空 key 内容相等却散列不等, 足以让 unordered_map 静默查不到。
TEST(PipelineLayoutKeyTest, MovedFromKeyStaysConsistent) {
    PipelineLayoutKey source = BuildKey(MakeTwoGroupDescriptor());
    const PipelineLayoutKey moved = std::move(source);

    ExpectSameKey(moved, BuildKey(MakeTwoGroupDescriptor()));
    ExpectSameKey(source, PipelineLayoutKey{});

    // 移动赋值同理。源带 push constant, 以便覆盖"optional 移动后仍 engaged"这一点。
    DescriptorBuilder withPushConstant = MakeTwoGroupDescriptor();
    withPushConstant.SetPushConstant(render::PushConstantDescriptor{
        .Location = {.Group = 2, .Binding = 0},
        .Size = 16,
        .Stages = render::ShaderStage::Vertex});
    PipelineLayoutKey assignTarget = BuildKey(MakeTwoGroupDescriptor());
    PipelineLayoutKey assignSource = BuildKey(withPushConstant);
    assignTarget = std::move(assignSource);
    ExpectSameKey(assignTarget, BuildKey(withPushConstant));
    ExpectSameKey(assignSource, PipelineLayoutKey{});
}

TEST(PipelineLayoutKeyTest, CopyPreservesContentAndHash) {
    const PipelineLayoutKey original = BuildKey(MakeTwoGroupDescriptor());
    const PipelineLayoutKey copy = original;
    ExpectSameKey(original, copy);

    // 拷贝必须是深的 —— entry 视图指向自己的存储, 不是原 key 的。
    ASSERT_EQ(copy.GetGroups().size(), original.GetGroups().size());
    EXPECT_NE(copy.GetEntries(copy.GetGroups()[0]).data(),
              original.GetEntries(original.GetGroups()[0]).data());
}

// ============================ 去重与引用计数 ============================

TEST_F(PipelineLayoutCacheTest, SameContentSharesOneLayout) {
    const DescriptorBuilder builder = MakeTwoGroupDescriptor();
    IntrusivePtr<SharedPipelineLayout> first = Cache().GetOrCreate(builder.Get());
    ASSERT_TRUE(first.HasValue());
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    EXPECT_EQ(Cache().GetMissCount(), 1u);
    EXPECT_EQ(Cache().GetHitCount(), 0u);
    EXPECT_EQ(first->GetRefCount(), 1u);
    EXPECT_TRUE(first->IsCached());

    // 另一份独立拼出的 descriptor —— 内容相同, 但没有任何指针关系。
    const DescriptorBuilder identical = MakeTwoGroupDescriptor();
    IntrusivePtr<SharedPipelineLayout> second = Cache().GetOrCreate(identical.Get());
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

    IntrusivePtr<SharedPipelineLayout> first = Cache().GetOrCreate(ordered.Get());
    ASSERT_TRUE(first.HasValue());
    IntrusivePtr<SharedPipelineLayout> second = Cache().GetOrCreate(shuffled.Get());
    ASSERT_TRUE(second.HasValue());
    EXPECT_EQ(second.Get(), first.Get());
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
}

/// 同组被拆成多个 parameter set 时也必须命中同一个 GPU 对象 —— 后端本来就会合并。
TEST_F(PipelineLayoutCacheTest, SplitParameterSetsShareOneLayout) {
    DescriptorBuilder split;
    split
        .AddGroup(0, {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)})
        .AddGroup(0, {MakeEntry(1, render::ShaderParameterBindingType::Texture, render::ShaderStage::Pixel)});

    DescriptorBuilder merged;
    merged.AddGroup(
        0,
        {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex),
         MakeEntry(1, render::ShaderParameterBindingType::Texture, render::ShaderStage::Pixel)});

    IntrusivePtr<SharedPipelineLayout> first = Cache().GetOrCreate(split.Get());
    ASSERT_TRUE(first.HasValue());
    IntrusivePtr<SharedPipelineLayout> second = Cache().GetOrCreate(merged.Get());
    ASSERT_TRUE(second.HasValue());
    EXPECT_EQ(second.Get(), first.Get());
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    EXPECT_EQ(Cache().GetHitCount(), 1u);
}

TEST_F(PipelineLayoutCacheTest, DifferentContentSplitsLayouts) {
    IntrusivePtr<SharedPipelineLayout> first = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
    ASSERT_TRUE(first.HasValue());

    DescriptorBuilder other;
    other.AddGroup(
        0,
        {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex)});
    IntrusivePtr<SharedPipelineLayout> second = Cache().GetOrCreate(other.Get());
    ASSERT_TRUE(second.HasValue());

    EXPECT_NE(second.Get(), first.Get());
    EXPECT_NE(second->Get(), first->Get());
    EXPECT_EQ(Cache().GetLayoutCount(), 2u);
    EXPECT_EQ(Cache().GetMissCount(), 2u);
    EXPECT_EQ(Cache().GetHitCount(), 0u);
}

TEST_F(PipelineLayoutCacheTest, LastReleaseDetachesFromCache) {
    {
        IntrusivePtr<SharedPipelineLayout> first = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
        ASSERT_TRUE(first.HasValue());
        {
            IntrusivePtr<SharedPipelineLayout> second = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
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
        IntrusivePtr<SharedPipelineLayout> first = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
        ASSERT_TRUE(first.HasValue());
    }
    ASSERT_EQ(Cache().GetLayoutCount(), 0u);

    IntrusivePtr<SharedPipelineLayout> again = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
    ASSERT_TRUE(again.HasValue());
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    EXPECT_EQ(Cache().GetMissCount(), 2u);
    EXPECT_EQ(Cache().GetHitCount(), 0u);
}

/// 【摘除必须只摘自己那一条】: Evict 走的是 key 反查, 一条摘错就会连带毁掉别人的 layout。
TEST_F(PipelineLayoutCacheTest, ReleasingOneEntryKeepsTheOthers) {
    DescriptorBuilder other;
    other.AddGroup(
        0,
        {MakeEntry(7, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Pixel)});

    IntrusivePtr<SharedPipelineLayout> keep = Cache().GetOrCreate(other.Get());
    ASSERT_TRUE(keep.HasValue());
    render::PipelineLayout* keepNative = keep->Get();
    {
        IntrusivePtr<SharedPipelineLayout> temporary = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
        ASSERT_TRUE(temporary.HasValue());
        ASSERT_EQ(Cache().GetLayoutCount(), 2u);
    }
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    EXPECT_TRUE(keep->IsCached());
    EXPECT_EQ(keep->Get(), keepNative);
    // 仍能命中, 说明留下的那条还在索引里而非只是对象没死。
    IntrusivePtr<SharedPipelineLayout> again = Cache().GetOrCreate(other.Get());
    EXPECT_EQ(again.Get(), keep.Get());
    EXPECT_EQ(Cache().GetHitCount(), 1u);
}

/// 热更新的形状: 布局没变时 reload 必须命中同一个 GPU 对象, 不重建 root signature。
TEST_F(PipelineLayoutCacheTest, ReloadWithUnchangedLayoutDoesNotRecreate) {
    IntrusivePtr<SharedPipelineLayout> oldRef = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
    ASSERT_TRUE(oldRef.HasValue());
    render::PipelineLayout* nativeBefore = oldRef->Get();
    const uint64_t missesBefore = Cache().GetMissCount();

    // 新 program 先取引用, 旧的再放开 —— reload 的真实顺序。
    IntrusivePtr<SharedPipelineLayout> newRef = Cache().GetOrCreate(MakeTwoGroupDescriptor().Get());
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
    IntrusivePtr<SharedPipelineLayout> ref = Cache().GetOrCreate(empty.Get());
    ASSERT_TRUE(ref.HasValue()) << "an empty layout is legal in both backends";
    EXPECT_EQ(Cache().GetLayoutCount(), 1u);
    EXPECT_NE(ref->Get(), nullptr);

    // 空 key 走的是默认构造那条路, 必须也能命中。
    IntrusivePtr<SharedPipelineLayout> again = Cache().GetOrCreate(empty.Get());
    EXPECT_EQ(again.Get(), ref.Get());
    EXPECT_EQ(Cache().GetHitCount(), 1u);
}

/// 建失败不该留下任何痕迹 —— 既不占表, 也不算 hit。重复 binding 在两个后端都被拒绝。
TEST_F(PipelineLayoutCacheTest, FailedCreationLeavesNoEntry) {
    DescriptorBuilder duplicate;
    duplicate.AddGroup(
        0,
        {MakeEntry(0, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Vertex),
         MakeEntry(0, render::ShaderParameterBindingType::Texture, render::ShaderStage::Pixel)});

    IntrusivePtr<SharedPipelineLayout> failed = Cache().GetOrCreate(duplicate.Get());
    EXPECT_FALSE(failed.HasValue()) << "duplicate bindings are rejected by both backends";
    EXPECT_EQ(Cache().GetLayoutCount(), 0u);
    EXPECT_EQ(Cache().GetHitCount(), 0u);
}

// ============================ 关停顺序 ============================

/// 【这是 Application 关停的常规路径, 不是异常路径】: RenderSystem (缓存的宿主) 先于
/// AssetManager 销毁, 而资产要到 AssetManager 析构时才放开最后一份 layout 引用。
TEST(PipelineLayoutCacheShutdownTest, CacheMayDieBeforeItsHolders) {
    DeviceContext ctx;
    if (!TryCreateAnyDevice(ctx)) {
        GTEST_SKIP() << "no render backend is available on this machine";
    }

    IntrusivePtr<SharedPipelineLayout> survivor;
    IntrusivePtr<SharedPipelineLayout> secondSurvivor;
    {
        PipelineLayoutCache cache{ctx.Device.get()};
        survivor = cache.GetOrCreate(MakeTwoGroupDescriptor().Get());
        ASSERT_TRUE(survivor.HasValue());

        DescriptorBuilder other;
        other.AddGroup(
            0,
            {MakeEntry(3, render::ShaderParameterBindingType::CBuffer, render::ShaderStage::Pixel)});
        secondSurvivor = cache.GetOrCreate(other.Get());
        ASSERT_TRUE(secondSurvivor.HasValue());
        EXPECT_EQ(cache.GetLayoutCount(), 2u);
        EXPECT_TRUE(survivor->IsCached());
    }
    // 缓存已死, layout 仍然可用 —— 引用计数保住了它。
    ASSERT_TRUE(survivor.HasValue());
    EXPECT_NE(survivor->Get(), nullptr);
    EXPECT_EQ(survivor->GetRefCount(), 1u);
    EXPECT_FALSE(survivor->IsCached()) << "the layout must know it is no longer indexed";
    EXPECT_NE(secondSurvivor->Get(), nullptr);

    // 之后归零走的是"已脱离缓存"那条分支, 直接销毁, 不回调已死的缓存。
    survivor.Reset();
    EXPECT_FALSE(survivor.HasValue());
    secondSurvivor.Reset();
}

TEST(PipelineLayoutCacheShutdownTest, EmptyCacheDiesCleanly) {
    DeviceContext ctx;
    if (!TryCreateAnyDevice(ctx)) {
        GTEST_SKIP() << "no render backend is available on this machine";
    }
    PipelineLayoutCache cache{ctx.Device.get()};
    EXPECT_EQ(cache.GetLayoutCount(), 0u);
    EXPECT_EQ(cache.GetDevice(), ctx.Device.get());
}

}  // namespace radray
