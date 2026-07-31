// RenderPassRegistry: 按 descriptor 去重 RenderPass 与 Framebuffer, 缓存独占所有权。
//
// 【覆盖重点】是四件事:
//   1. key 的相等性携带正确的语义 —— 尤其是 ColorAttachments 的【顺序有意义】, 这与
//      PipelineLayoutKey 的归一化策略正好相反, 是最容易被后来者"顺手排个序"改坏的地方;
//   2. key 满足 unordered_map 的契约 —— 内容相等必然散列相等;
//   3. descriptor 能无损回读 (Get()), 因为 key 是 span 版 descriptor 的持有化改写;
//   4. 缓存的去重与摘除 —— 同 descriptor 命中同一对象, RemoveFramebuffersUsing 只摘走
//      引用了给定 view 的条目且不碰 render pass。
//
// 1-3 是纯 CPU 数据, 无条件运行。4 需要真实 device: RenderPass / Framebuffer 是 GPU 对象
// (D3D12 的 RTV 描述 / Vulkan 的 VkRenderPass + VkFramebuffer), 没有可替换的假实现,
// 无设备时 GTEST_SKIP。

#include <radray/render/render_pass_registry.h>

#include <radray/render/rhi.h>
#include <radray/types.h>

#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif
#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif

#include <gtest/gtest.h>

#include <optional>
#include <span>

namespace radray::render {
namespace {

/// 一个 device。本文件只建 texture / view / pass / framebuffer, 不提交命令, 故不要队列。
struct DeviceContext {
    bool VulkanEnvInitialized{false};
    unique_ptr<DXGIFactory> Factory;
    shared_ptr<Device> Device;

    ~DeviceContext() {
        Device.reset();
        Factory.reset();
#if defined(RADRAY_ENABLE_VULKAN)
        if (VulkanEnvInitialized) {
            InstanceVulkan::ShutdownEnv();
        }
#endif
    }
};

bool TryCreateAnyDevice(DeviceContext& ctx) {
#if defined(RADRAY_ENABLE_D3D12)
    {
        DXGIFactoryDescriptor factoryDesc{};
        factoryDesc.IsEnableDebugLayer = false;
        auto factory = DXGIFactory::Create(factoryDesc);
        if (factory.HasValue()) {
            ctx.Factory = factory.Release();
            D3D12DeviceDescriptor d3d12Desc{};
            d3d12Desc.Factory = ctx.Factory.get();
            auto device = Device::Create(DeviceDescriptor{d3d12Desc});
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
        VulkanInstanceDescriptor instanceDesc{};
        instanceDesc.AppName = "radray_render_pass_registry_test";
        instanceDesc.EngineName = "radray";
        instanceDesc.IsEnableDebugLayer = false;
        if (InstanceVulkan::InitEnv(instanceDesc)) {
            ctx.VulkanEnvInitialized = true;
            VulkanDeviceDescriptor vkDesc{};
            auto device = Device::Create(DeviceDescriptor{vkDesc});
            if (device.HasValue()) {
                ctx.Device = device.Release();
                return true;
            }
        }
    }
#endif
    return false;
}

constexpr uint32_t kWidth = 64;
constexpr uint32_t kHeight = 32;

RenderPassColorAttachmentDescriptor MakeColor(
    TextureFormat format,
    LoadAction load = LoadAction::Clear) noexcept {
    RenderPassColorAttachmentDescriptor attachment{};
    attachment.Format = format;
    attachment.SampleCount = 1;
    attachment.Load = load;
    attachment.Store = StoreAction::Store;
    return attachment;
}

RenderPassDepthStencilAttachmentDescriptor MakeDepth() noexcept {
    RenderPassDepthStencilAttachmentDescriptor attachment{};
    attachment.Format = TextureFormat::D32_FLOAT;
    attachment.SampleCount = 1;
    attachment.DepthLoad = LoadAction::Clear;
    attachment.DepthStore = StoreAction::Store;
    attachment.StencilLoad = LoadAction::DontCare;
    attachment.StencilStore = StoreAction::Discard;
    return attachment;
}

/// 相等的两个 key 必须散列相等 —— unordered_map 的硬性契约, 违反即静默查不到。
template <class Key>
void ExpectSameKey(const Key& lhs, const Key& rhs) {
    EXPECT_TRUE(lhs == rhs);
    EXPECT_EQ(std::hash<Key>{}(lhs), std::hash<Key>{}(rhs)) << "equal keys must hash equal";
}

template <class Key>
void ExpectDifferentKey(const Key& lhs, const Key& rhs) {
    EXPECT_FALSE(lhs == rhs);
}

/// 一张可当渲染目标的纹理 + 它的 RTV。framebuffer 需要真 view。
struct RenderTarget {
    unique_ptr<Texture> Tex;
    unique_ptr<TextureView> View;
};

std::optional<RenderTarget> MakeRenderTarget(Device* device, TextureFormat format) {
    TextureDescriptor texDesc{};
    texDesc.Dim = TextureDimension::Dim2D;
    texDesc.Width = kWidth;
    texDesc.Height = kHeight;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.SampleCount = 1;
    texDesc.Format = format;
    texDesc.Memory = MemoryType::Device;
    texDesc.Usage = TextureUse::RenderTarget;
    auto tex = device->CreateTexture(texDesc);
    if (!tex.HasValue()) {
        return std::nullopt;
    }
    RenderTarget target{};
    target.Tex = tex.Release();

    TextureViewDescriptor viewDesc{};
    viewDesc.Target = target.Tex.get();
    viewDesc.Dim = TextureDimension::Dim2D;
    viewDesc.Format = format;
    viewDesc.Range = SubresourceRange{
        .BaseArrayLayer = 0,
        .ArrayLayerCount = 1,
        .BaseMipLevel = 0,
        .MipLevelCount = 1};
    viewDesc.Usage = TextureViewUsage::RenderTarget;
    auto view = device->CreateTextureView(viewDesc);
    if (!view.HasValue()) {
        return std::nullopt;
    }
    target.View = view.Release();
    return target;
}

class RenderPassRegistryTest : public testing::Test {
protected:
    void SetUp() override {
        if (!TryCreateAnyDevice(_ctx)) {
            GTEST_SKIP() << "no render backend is available on this machine";
        }
        _registry = make_unique<RenderPassRegistry>(_ctx.Device.get());
    }

    void TearDown() override { _registry.reset(); }

    RenderPassRegistry& Registry() { return *_registry; }
    Device* GetDevice() const noexcept { return _ctx.Device.get(); }

    /// 单个 RGBA8 颜色附件的 pass。建不出来时返回 nullptr, 由调用方 SKIP。
    Nullable<RenderPass*> GetSingleColorPass() {
        const auto color = MakeColor(TextureFormat::RGBA8_UNORM);
        const RenderPassDescriptor desc{.ColorAttachments = std::span{&color, 1}};
        return _registry->GetOrCreateRenderPass(desc);
    }

    Nullable<Framebuffer*> GetFramebuffer(RenderPass* pass, TextureView* view) {
        const FramebufferDescriptor desc{
            .Pass = pass,
            .ColorAttachments = std::span<TextureView* const>{&view, 1},
            .DepthStencilAttachment = nullptr,
            .Width = kWidth,
            .Height = kHeight};
        return _registry->GetOrCreateFramebuffer(desc);
    }

private:
    DeviceContext _ctx;
    unique_ptr<RenderPassRegistry> _registry;
};

}  // namespace

// ======================== RenderPassCacheKey (纯 CPU) ========================

TEST(RenderPassCacheKeyTest, SameDescriptorProducesEqualKey) {
    const RenderPassColorAttachmentDescriptor colors[]{
        MakeColor(TextureFormat::RGBA8_UNORM),
        MakeColor(TextureFormat::RG16_FLOAT)};
    const RenderPassDescriptor desc{
        .ColorAttachments = colors,
        .DepthStencilAttachment = MakeDepth()};

    // 两次 Build 走的是两份独立的 vector 副本, 相等性必须看内容而非缓冲地址。
    ExpectSameKey(RenderPassCacheKey::Build(desc), RenderPassCacheKey::Build(desc));
}

/// 本类最重要的一条: 颜色附件顺序【就是】RTV 槽位, 交换即不同的 pass。
/// 若哪天有人给 key 加了排序归一化, 这条会红。
TEST(RenderPassCacheKeyTest, ColorAttachmentOrderIsSignificant) {
    const auto rgba = MakeColor(TextureFormat::RGBA8_UNORM);
    const auto rg = MakeColor(TextureFormat::RG16_FLOAT);

    const RenderPassColorAttachmentDescriptor forward[]{rgba, rg};
    const RenderPassColorAttachmentDescriptor swapped[]{rg, rgba};

    ExpectDifferentKey(
        RenderPassCacheKey::Build(RenderPassDescriptor{.ColorAttachments = forward}),
        RenderPassCacheKey::Build(RenderPassDescriptor{.ColorAttachments = swapped}));
}

TEST(RenderPassCacheKeyTest, DistinguishesRealDifferences) {
    const auto baseColor = MakeColor(TextureFormat::RGBA8_UNORM);
    const RenderPassCacheKey base = RenderPassCacheKey::Build(
        RenderPassDescriptor{
            .ColorAttachments = std::span{&baseColor, 1},
            .DepthStencilAttachment = MakeDepth()});

    {  // 附件格式不同
        const auto color = MakeColor(TextureFormat::BGRA8_UNORM);
        ExpectDifferentKey(
            base,
            RenderPassCacheKey::Build(
                RenderPassDescriptor{
                    .ColorAttachments = std::span{&color, 1},
                    .DepthStencilAttachment = MakeDepth()}));
    }
    {  // load action 不同: Clear 与 Load 在后端是不同的 pass
        const auto color = MakeColor(TextureFormat::RGBA8_UNORM, LoadAction::Load);
        ExpectDifferentKey(
            base,
            RenderPassCacheKey::Build(
                RenderPassDescriptor{
                    .ColorAttachments = std::span{&color, 1},
                    .DepthStencilAttachment = MakeDepth()}));
    }
    {  // 少了深度附件
        ExpectDifferentKey(
            base,
            RenderPassCacheKey::Build(
                RenderPassDescriptor{.ColorAttachments = std::span{&baseColor, 1}}));
    }
    {  // 多了一个颜色附件
        const RenderPassColorAttachmentDescriptor colors[]{baseColor, baseColor};
        ExpectDifferentKey(
            base,
            RenderPassCacheKey::Build(
                RenderPassDescriptor{
                    .ColorAttachments = colors,
                    .DepthStencilAttachment = MakeDepth()}));
    }
}

TEST(RenderPassCacheKeyTest, GetRoundTripsDescriptor) {
    const RenderPassColorAttachmentDescriptor colors[]{
        MakeColor(TextureFormat::RGBA8_UNORM),
        MakeColor(TextureFormat::RG16_FLOAT, LoadAction::Load)};
    const RenderPassDescriptor desc{
        .ColorAttachments = colors,
        .DepthStencilAttachment = MakeDepth()};

    const RenderPassCacheKey key = RenderPassCacheKey::Build(desc);
    const RenderPassDescriptor readBack = key.Get();

    ASSERT_EQ(readBack.ColorAttachments.size(), 2u);
    EXPECT_EQ(readBack.ColorAttachments[0], colors[0]);
    EXPECT_EQ(readBack.ColorAttachments[1], colors[1]);
    EXPECT_EQ(readBack.DepthStencilAttachment, desc.DepthStencilAttachment);
    // 回读的 descriptor 必须能重建出同一个 key, 否则 GetDesc 型接口会撕裂缓存。
    ExpectSameKey(key, RenderPassCacheKey::Build(readBack));
}

TEST(RenderPassCacheKeyTest, DefaultKeyEqualsBuiltEmptyKey) {
    // depth-only / 空 pass 是合法输入, 默认构造的 key 不能和它撞或错开。
    ExpectSameKey(RenderPassCacheKey{}, RenderPassCacheKey::Build(RenderPassDescriptor{}));
}

// ======================== FramebufferCacheKey (纯 CPU) ========================

namespace {

/// 只用作身份比较的假 view 地址。key 只存指针、不解引用, 故无需真对象。
TextureView* FakeView(uintptr_t id) noexcept {
    return reinterpret_cast<TextureView*>(id * alignof(std::max_align_t));
}

RenderPass* FakePass(uintptr_t id) noexcept {
    return reinterpret_cast<RenderPass*>(id * alignof(std::max_align_t));
}

FramebufferDescriptor MakeFramebufferDesc(
    RenderPass* pass,
    std::span<TextureView* const> colors,
    TextureView* depthStencil = nullptr) noexcept {
    return FramebufferDescriptor{
        .Pass = pass,
        .ColorAttachments = colors,
        .DepthStencilAttachment = depthStencil,
        .Width = kWidth,
        .Height = kHeight,
        .Layers = 1};
}

}  // namespace

TEST(FramebufferCacheKeyTest, SameDescriptorProducesEqualKey) {
    TextureView* views[]{FakeView(1), FakeView(2)};
    const FramebufferDescriptor desc = MakeFramebufferDesc(FakePass(1), views, FakeView(3));

    ExpectSameKey(FramebufferCacheKey::Build(desc), FramebufferCacheKey::Build(desc));
}

/// 同 render pass 的理由: 附件下标就是槽位, 交换后写入目标不同。
TEST(FramebufferCacheKeyTest, ColorAttachmentOrderIsSignificant) {
    TextureView* forward[]{FakeView(1), FakeView(2)};
    TextureView* swapped[]{FakeView(2), FakeView(1)};
    RenderPass* pass = FakePass(1);

    ExpectDifferentKey(
        FramebufferCacheKey::Build(MakeFramebufferDesc(pass, forward)),
        FramebufferCacheKey::Build(MakeFramebufferDesc(pass, swapped)));
}

TEST(FramebufferCacheKeyTest, DistinguishesRealDifferences) {
    TextureView* views[]{FakeView(1)};
    RenderPass* pass = FakePass(1);
    const FramebufferCacheKey base =
        FramebufferCacheKey::Build(MakeFramebufferDesc(pass, views, FakeView(9)));

    {  // pass 不同
        ExpectDifferentKey(
            base,
            FramebufferCacheKey::Build(MakeFramebufferDesc(FakePass(2), views, FakeView(9))));
    }
    {  // 颜色附件换了 view
        TextureView* other[]{FakeView(2)};
        ExpectDifferentKey(
            base,
            FramebufferCacheKey::Build(MakeFramebufferDesc(pass, other, FakeView(9))));
    }
    {  // 深度附件不同
        ExpectDifferentKey(
            base,
            FramebufferCacheKey::Build(MakeFramebufferDesc(pass, views, FakeView(10))));
    }
    {  // 尺寸不同: 交换链改尺寸后必须建新 framebuffer, 不能复用
        FramebufferDescriptor desc = MakeFramebufferDesc(pass, views, FakeView(9));
        desc.Width = kWidth * 2;
        ExpectDifferentKey(base, FramebufferCacheKey::Build(desc));
    }
    {  // 层数不同
        FramebufferDescriptor desc = MakeFramebufferDesc(pass, views, FakeView(9));
        desc.Layers = 2;
        ExpectDifferentKey(base, FramebufferCacheKey::Build(desc));
    }
}

TEST(FramebufferCacheKeyTest, GetRoundTripsDescriptor) {
    TextureView* views[]{FakeView(1), FakeView(2)};
    const FramebufferDescriptor desc = MakeFramebufferDesc(FakePass(7), views, FakeView(3));

    const FramebufferCacheKey key = FramebufferCacheKey::Build(desc);
    const FramebufferDescriptor readBack = key.Get();

    EXPECT_EQ(readBack.Pass, desc.Pass);
    ASSERT_EQ(readBack.ColorAttachments.size(), 2u);
    EXPECT_EQ(readBack.ColorAttachments[0], views[0]);
    EXPECT_EQ(readBack.ColorAttachments[1], views[1]);
    EXPECT_EQ(readBack.DepthStencilAttachment, desc.DepthStencilAttachment);
    EXPECT_EQ(readBack.Width, desc.Width);
    EXPECT_EQ(readBack.Height, desc.Height);
    EXPECT_EQ(readBack.Layers, desc.Layers);
    ExpectSameKey(key, FramebufferCacheKey::Build(readBack));
}

/// References 是 RemoveFramebuffersUsing 的判据, 颜色与深度两侧都要认。
TEST(FramebufferCacheKeyTest, ReferencesFindsBothColorAndDepth) {
    TextureView* views[]{FakeView(1), FakeView(2)};
    const FramebufferCacheKey key =
        FramebufferCacheKey::Build(MakeFramebufferDesc(FakePass(1), views, FakeView(3)));

    EXPECT_TRUE(key.References(FakeView(1)));
    EXPECT_TRUE(key.References(FakeView(2)));
    EXPECT_TRUE(key.References(FakeView(3)));
    EXPECT_FALSE(key.References(FakeView(4)));
    // 空 view 不能匹配"没有深度附件"的条目, 否则会误摘一片。
    EXPECT_FALSE(key.References(nullptr));
}

TEST(FramebufferCacheKeyTest, NullDepthDoesNotMatchNullQuery) {
    TextureView* views[]{FakeView(1)};
    const FramebufferCacheKey key =
        FramebufferCacheKey::Build(MakeFramebufferDesc(FakePass(1), views, nullptr));

    EXPECT_FALSE(key.References(nullptr));
}

// ======================== 缓存行为 (需要 device) ========================

TEST_F(RenderPassRegistryTest, DeviceIsKept) {
    EXPECT_EQ(Registry().GetDevice(), GetDevice());
}

TEST_F(RenderPassRegistryTest, SameDescriptorReusesRenderPass) {
    auto first = GetSingleColorPass();
    if (!first.HasValue()) {
        GTEST_SKIP() << "backend cannot create a simple color render pass";
    }
    auto second = GetSingleColorPass();
    ASSERT_TRUE(second.HasValue());

    EXPECT_EQ(first.Get(), second.Get());
    EXPECT_EQ(Registry().GetRenderPassCount(), 1u);
    EXPECT_EQ(Registry().GetRenderPassMissCount(), 1u);
    EXPECT_EQ(Registry().GetRenderPassHitCount(), 1u);
}

TEST_F(RenderPassRegistryTest, DifferentDescriptorCreatesNewRenderPass) {
    auto rgba = GetSingleColorPass();
    if (!rgba.HasValue()) {
        GTEST_SKIP() << "backend cannot create a simple color render pass";
    }
    const auto other = MakeColor(TextureFormat::RGBA8_UNORM, LoadAction::Load);
    auto loaded = Registry().GetOrCreateRenderPass(
        RenderPassDescriptor{.ColorAttachments = std::span{&other, 1}});
    ASSERT_TRUE(loaded.HasValue());

    EXPECT_NE(rgba.Get(), loaded.Get());
    EXPECT_EQ(Registry().GetRenderPassCount(), 2u);
    EXPECT_EQ(Registry().GetRenderPassMissCount(), 2u);
    EXPECT_EQ(Registry().GetRenderPassHitCount(), 0u);
}

TEST_F(RenderPassRegistryTest, SameDescriptorReusesFramebuffer) {
    auto pass = GetSingleColorPass();
    if (!pass.HasValue()) {
        GTEST_SKIP() << "backend cannot create a simple color render pass";
    }
    auto target = MakeRenderTarget(GetDevice(), TextureFormat::RGBA8_UNORM);
    if (!target.has_value()) {
        GTEST_SKIP() << "backend cannot create a render target texture/view";
    }

    auto first = GetFramebuffer(pass.Get(), target->View.get());
    if (!first.HasValue()) {
        GTEST_SKIP() << "backend cannot create a framebuffer";
    }
    auto second = GetFramebuffer(pass.Get(), target->View.get());
    ASSERT_TRUE(second.HasValue());

    EXPECT_EQ(first.Get(), second.Get());
    EXPECT_EQ(Registry().GetFramebufferCount(), 1u);
    EXPECT_EQ(Registry().GetFramebufferMissCount(), 1u);
    EXPECT_EQ(Registry().GetFramebufferHitCount(), 1u);
}

/// 交换链换尺寸的路径: 摘掉引用旧 view 的 framebuffer, 但 render pass 必须留着 ——
/// pass 不引用任何 view, 重建它是纯浪费。
TEST_F(RenderPassRegistryTest, RemoveFramebuffersUsingKeepsRenderPasses) {
    auto pass = GetSingleColorPass();
    if (!pass.HasValue()) {
        GTEST_SKIP() << "backend cannot create a simple color render pass";
    }
    auto kept = MakeRenderTarget(GetDevice(), TextureFormat::RGBA8_UNORM);
    auto dropped = MakeRenderTarget(GetDevice(), TextureFormat::RGBA8_UNORM);
    if (!kept.has_value() || !dropped.has_value()) {
        GTEST_SKIP() << "backend cannot create a render target texture/view";
    }

    auto keptFb = GetFramebuffer(pass.Get(), kept->View.get());
    auto droppedFb = GetFramebuffer(pass.Get(), dropped->View.get());
    if (!keptFb.HasValue() || !droppedFb.HasValue()) {
        GTEST_SKIP() << "backend cannot create a framebuffer";
    }
    ASSERT_EQ(Registry().GetFramebufferCount(), 2u);

    EXPECT_EQ(Registry().RemoveFramebuffersUsing(dropped->View.get()), 1u);
    EXPECT_EQ(Registry().GetFramebufferCount(), 1u);
    EXPECT_EQ(Registry().GetRenderPassCount(), 1u);

    // 留下的那个仍应命中, 而不是被顺手摘掉后重建。
    const uint64_t missesBefore = Registry().GetFramebufferMissCount();
    auto again = GetFramebuffer(pass.Get(), kept->View.get());
    ASSERT_TRUE(again.HasValue());
    EXPECT_EQ(again.Get(), keptFb.Get());
    EXPECT_EQ(Registry().GetFramebufferMissCount(), missesBefore);
}

TEST_F(RenderPassRegistryTest, RemoveFramebuffersUsingUnknownViewRemovesNothing) {
    auto pass = GetSingleColorPass();
    if (!pass.HasValue()) {
        GTEST_SKIP() << "backend cannot create a simple color render pass";
    }
    auto target = MakeRenderTarget(GetDevice(), TextureFormat::RGBA8_UNORM);
    if (!target.has_value()) {
        GTEST_SKIP() << "backend cannot create a render target texture/view";
    }
    auto framebuffer = GetFramebuffer(pass.Get(), target->View.get());
    if (!framebuffer.HasValue()) {
        GTEST_SKIP() << "backend cannot create a framebuffer";
    }

    auto unrelated = MakeRenderTarget(GetDevice(), TextureFormat::RGBA8_UNORM);
    ASSERT_TRUE(unrelated.has_value());
    EXPECT_EQ(Registry().RemoveFramebuffersUsing(unrelated->View.get()), 0u);
    EXPECT_EQ(Registry().RemoveFramebuffersUsing(nullptr), 0u);
    EXPECT_EQ(Registry().GetFramebufferCount(), 1u);
}

TEST_F(RenderPassRegistryTest, ClearFramebuffersKeepsRenderPasses) {
    auto pass = GetSingleColorPass();
    if (!pass.HasValue()) {
        GTEST_SKIP() << "backend cannot create a simple color render pass";
    }
    auto target = MakeRenderTarget(GetDevice(), TextureFormat::RGBA8_UNORM);
    if (!target.has_value()) {
        GTEST_SKIP() << "backend cannot create a render target texture/view";
    }
    if (!GetFramebuffer(pass.Get(), target->View.get()).HasValue()) {
        GTEST_SKIP() << "backend cannot create a framebuffer";
    }

    Registry().ClearFramebuffers();
    EXPECT_EQ(Registry().GetFramebufferCount(), 0u);
    EXPECT_EQ(Registry().GetRenderPassCount(), 1u);

    // 清完还能继续用, 且 pass 仍命中原对象。
    auto again = GetSingleColorPass();
    ASSERT_TRUE(again.HasValue());
    EXPECT_EQ(again.Get(), pass.Get());
}

/// Clear 只清空缓存, 不置空 device —— 之后必须还能建东西。
TEST_F(RenderPassRegistryTest, ClearLeavesRegistryUsable) {
    if (!GetSingleColorPass().HasValue()) {
        GTEST_SKIP() << "backend cannot create a simple color render pass";
    }
    Registry().Clear();
    EXPECT_EQ(Registry().GetRenderPassCount(), 0u);
    EXPECT_EQ(Registry().GetFramebufferCount(), 0u);
    EXPECT_EQ(Registry().GetDevice(), GetDevice());

    auto rebuilt = GetSingleColorPass();
    EXPECT_TRUE(rebuilt.HasValue());
    EXPECT_EQ(Registry().GetRenderPassCount(), 1u);
}

}  // namespace radray::render
