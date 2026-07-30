// TextureContent / StaticMeshContent: 内容与槽位分离后的生命周期。
//
// 【覆盖重点】是分离要买到的那三条性质, 对每种内容各验一遍:
//   1. Unload 只销毁槽位 —— 有人持有内容时 GPU 资源一个都不许交出去;
//   2. 归零【无条件】走 recycler —— 这是 StaticMesh 从前用 use_count() == 1 时漏掉的
//      分支, 恰恰是唯一有 GPU 危险的那条 (GPU 可能仍在读, 不等 fence 就析构);
//      注意被否定的是"在生产的释放逻辑里拿 use_count() 做分支", 而非这个函数本身 ——
//      本文件的断言照样用 use_count() 观察计数, 那只是观察, 不改变任何释放路径;
//   3. 内容可以比 AssetManager 活得久 —— AssetContentDeleter 自持 recycler 指针的全部理由。
//
// 需要真实 device: Texture / TextureView / Buffer 都是 GPU 对象, 没有可替换的假实现。
// 无设备时 GTEST_SKIP。

#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>

#include <radray/render/rhi.h>
#include <radray/runtime/render_resource_recycler.h>
#include <radray/types.h>

#include <gtest/gtest.h>

namespace radray {
namespace {

/// 一个 device。本文件只建资源, 不提交命令, 故不要队列。
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
        instanceDesc.AppName = "radray_asset_content_test";
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

/// 只数交出来了几个对象。数量本身就是断言的对象 —— "交出去了几个"与"该交几个"
/// 逐一对齐才能证明没有资源绕过 recycler 直接析构。
class CountingRecycler : public IRenderResourceRecycler {
public:
    void RecycleRenderResource(unique_ptr<render::RenderBase> obj) noexcept override {
        if (obj != nullptr) {
            ++Count;
        }
    }

    uint32_t Count{0};
};

class AssetContentTest : public testing::Test {
protected:
    void SetUp() override {
        if (!TryCreateAnyDevice(_ctx)) {
            GTEST_SKIP() << "no render backend is available on this machine";
        }
        _assets.SetRecycler(&_recycler);
    }

    render::Device& Device() { return *_ctx.Device; }
    AssetManager& Assets() { return _assets; }
    CountingRecycler& Recycler() { return _recycler; }

    /// 一张 1x1 贴图 + 默认全量 SRV。不上传像素 —— 本文件只关心生命周期, 内容是什么无关。
    shared_ptr<TextureContent> MakeTexture(AssetManager& assets, std::string_view name) {
        constexpr render::TextureFormat format = render::TextureFormat::RGBA8_UNORM;
        render::TextureDescriptor texDesc{
            .Dim = render::TextureDimension::Dim2D,
            .Width = 1,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleCount = 1,
            .Format = format,
            .Memory = render::MemoryType::Device,
            .Usage = render::TextureUse::Resource | render::TextureUse::CopyDestination,
            .Hints = render::ResourceHint::None};
        auto texture = Device().CreateTexture(texDesc);
        if (!texture.HasValue()) {
            return {};
        }
        auto tex = texture.Release();
        render::TextureViewDescriptor viewDesc{
            .Target = tex.get(),
            .Dim = render::TextureDimension::Dim2D,
            .Format = format,
            .Range = render::SubresourceRange::AllSub(),
            .Usage = render::TextureViewUsage::Resource};
        auto srv = Device().CreateTextureView(viewDesc);
        if (!srv.HasValue()) {
            return {};
        }
        return assets.MakeContent<TextureContent>(
            &Device(), string{name}, std::move(tex), srv.Release());
    }

    /// 一份只有 GPU buffer 的网格内容。CPU 侧 MeshResource 留空 —— IsValid 会因此为
    /// false, 但本文件不测校验, 只测"buffer 归零时怎么走"。
    shared_ptr<StaticMeshContent> MakeMesh(AssetManager& assets, uint32_t bufferCount) {
        GpuMesh mesh;
        for (uint32_t i = 0; i < bufferCount; ++i) {
            render::BufferDescriptor desc{
                .Size = 256,
                .Memory = render::MemoryType::Device,
                .Usage = render::BufferUse::Vertex | render::BufferUse::CopyDestination,
                .Hints = render::ResourceHint::None};
            auto buffer = Device().CreateBuffer(desc);
            if (!buffer.HasValue()) {
                return {};
            }
            mesh.Buffers.push_back(buffer.Release());
        }
        GpuMesh::DrawData draw{};
        draw.Vbv.Target = mesh.Buffers.empty() ? nullptr : mesh.Buffers[0].get();
        mesh.Draws.push_back(draw);
        return assets.MakeContent<StaticMeshContent>(
            MeshResource{},
            vector<StaticMeshSection>{},
            Eigen::Vector3f::Zero(),
            Eigen::Vector3f::Zero(),
            std::move(mesh));
    }

private:
    DeviceContext _ctx;
    CountingRecycler _recycler;
    /// 【必须声明在 _recycler 之后】: 内容归零时把 GPU 对象交给 recycler, 析构逆序保证
    /// manager (及其持有的资产) 先死。
    AssetManager _assets;
};

// ============================ TextureContent ============================

TEST_F(AssetContentTest, TextureUnloadKeepsContentAliveWhileHeld) {
    shared_ptr<TextureContent> content = MakeTexture(Assets(), "held");
    ASSERT_TRUE(content != nullptr);
    ASSERT_TRUE(content->IsValid());
    render::TextureView* srv = content->GetSrv();
    ASSERT_NE(srv, nullptr);

    auto asset = make_unique<TextureAsset>(content);
    EXPECT_EQ(content.use_count(), 2) << "asset's one + this local one";

    CountingRecycler unloadRecycler;
    asset->OnUnload(unloadRecycler);
    EXPECT_FALSE(asset->HasContent()) << "the slot dropped its reference";
    // 【核心断言】: 槽位死了, 但一个 GPU 对象都不许交出去 —— 描述符里还存着这个 srv。
    EXPECT_EQ(unloadRecycler.Count, 0u) << "no GPU object may be released while held";
    EXPECT_EQ(Recycler().Count, 0u);

    EXPECT_EQ(content.use_count(), 1);
    EXPECT_TRUE(content->IsValid());
    EXPECT_EQ(content->GetSrv(), srv) << "the view pointer stays valid, not just non-null";
}

TEST_F(AssetContentTest, TextureReleasesEveryViewThroughTheRecycler) {
    shared_ptr<TextureContent> content = MakeTexture(Assets(), "views");
    ASSERT_TRUE(content != nullptr);

    // 建两个非默认子 view。它们进 _viewCache, 归零时必须一并交出。
    render::TextureView* mip = content->GetOrCreateSrv(TextureSubViewDesc{
        .Range = render::SubresourceRange{0, 1, 0, 1}});
    ASSERT_NE(mip, nullptr);
    render::TextureView* again = content->GetOrCreateSrv(TextureSubViewDesc{
        .Range = render::SubresourceRange{0, 1, 0, 1}});
    EXPECT_EQ(again, mip) << "the same descriptor must hit the cache, not create a second view";

    const uint32_t before = Recycler().Count;
    content.reset();
    // texture + 默认 srv + 一个子 view = 3。少一个就说明有对象绕过 recycler 直接析构了。
    EXPECT_EQ(Recycler().Count - before, 3u);
}

TEST_F(AssetContentTest, TextureContentOutlivesTheAssetManagerItself) {
    // 【为何这条必须成立】: AssetContentDeleter 自持 recycler 指针就是为它。若归零时回头向
    // AssetManager 索取 recycler, 这里会解引用一个已析构的对象。
    CountingRecycler recycler;
    shared_ptr<TextureContent> held;
    {
        AssetManager assets;
        assets.SetRecycler(&recycler);
        held = MakeTexture(assets, "orphan");
        ASSERT_TRUE(held != nullptr);
        auto asset = make_unique<TextureAsset>(held);
        (void)asset;  // 资产随作用域一起死, 内容不受影响。
    }
    ASSERT_EQ(held.use_count(), 1);
    EXPECT_TRUE(held->IsValid());
    EXPECT_EQ(recycler.Count, 0u);

    held.reset();
    EXPECT_EQ(recycler.Count, 2u) << "texture + default srv, through the content's own recycler";
}

// ============================ StaticMeshContent ============================

TEST_F(AssetContentTest, MeshUnloadKeepsContentAliveWhileHeld) {
    shared_ptr<StaticMeshContent> content = MakeMesh(Assets(), /*bufferCount*/ 2);
    ASSERT_TRUE(content != nullptr);
    // SceneProxy 缓存的正是这个指针 (见 MeshDrawArgs::Geometry)。
    const GpuMesh::DrawData* geometry = content->GetRenderMesh().Draws.data();
    ASSERT_NE(geometry, nullptr);

    auto asset = make_unique<StaticMesh>(content);
    EXPECT_EQ(content.use_count(), 2);

    CountingRecycler unloadRecycler;
    asset->OnUnload(unloadRecycler);
    EXPECT_FALSE(asset->HasContent());
    EXPECT_EQ(unloadRecycler.Count, 0u) << "no buffer may be released while a proxy holds it";

    EXPECT_EQ(content.use_count(), 1);
    EXPECT_EQ(content->GetRenderMesh().Draws.data(), geometry);
    EXPECT_EQ(content->GetRenderMesh().Buffers.size(), 2u);
}

/// 【这条守的正是从前那个洞】: 分离前 StaticMesh::OnUnload 写着
/// `if (_renderMesh.use_count() == 1)`, 于是"别人还持有"时 buffer 绕过 recycler,
/// 在最后一个 shared_ptr 归零处立即析构 —— 不等 fence。归零交给 AssetContentDeleter 后, 到
/// ReleaseRenderResources 必然是唯一所有者, 无需也不该再判断。
///
/// 【别把下面的 use_count() 断言与那个洞混为一谈】: 洞在于【生产的释放逻辑】拿 use_count()
/// 当分支条件, 测试只是在旁观察计数, 不参与任何释放决策。
TEST_F(AssetContentTest, MeshReleasesEveryBufferThroughTheRecyclerEvenWhenShared) {
    shared_ptr<StaticMeshContent> content = MakeMesh(Assets(), /*bufferCount*/ 3);
    ASSERT_TRUE(content != nullptr);

    // 制造"多个持有者"这个从前会走进坏分支的局面。
    shared_ptr<StaticMeshContent> second = content;
    auto asset = make_unique<StaticMesh>(content);
    EXPECT_EQ(content.use_count(), 3);

    CountingRecycler unloadRecycler;
    asset->OnUnload(unloadRecycler);
    second.reset();
    EXPECT_EQ(unloadRecycler.Count, 0u);
    EXPECT_EQ(Recycler().Count, 0u) << "still held by `content`";

    const uint32_t before = Recycler().Count;
    content.reset();
    EXPECT_EQ(Recycler().Count - before, 3u) << "every buffer must go through the recycler";
}

TEST_F(AssetContentTest, MeshContentOutlivesTheAssetManagerItself) {
    CountingRecycler recycler;
    shared_ptr<StaticMeshContent> held;
    {
        AssetManager assets;
        assets.SetRecycler(&recycler);
        held = MakeMesh(assets, /*bufferCount*/ 1);
        ASSERT_TRUE(held != nullptr);
    }
    ASSERT_EQ(held.use_count(), 1);
    EXPECT_EQ(held->GetRenderMesh().Buffers.size(), 1u);
    EXPECT_EQ(recycler.Count, 0u);

    held.reset();
    EXPECT_EQ(recycler.Count, 1u);
}

/// CPU 侧校验不再需要构造一个空资产当 probe —— 它只依赖数据。
TEST(StaticMeshDataTest, EmptyResourceIsRejected) {
    EXPECT_FALSE(IsStaticMeshDataValid(MeshResource{}, {}));
}

}  // namespace
}  // namespace radray
