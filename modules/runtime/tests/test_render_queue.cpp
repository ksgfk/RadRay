#include <gtest/gtest.h>

#include <radray/runtime/render_framework/render_queue.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>

using namespace radray;

namespace {

class FakeProxy : public PrimitiveSceneProxy {
public:
    FakeProxy() noexcept = default;
    ~FakeProxy() noexcept override = default;
};

ShaderAsset MakeShader(std::string_view passTag) {
    ShaderKeywordSet kw;
    vector<ShaderPassDesc> passes;
    passes.push_back(ShaderPassDesc{.PassTag = string{passTag}, .Source = ""});
    return ShaderAsset{std::move(kw), std::move(passes)};
}

ShaderAsset MakeShaderTwoPasses(std::string_view tag0, std::string_view tag1) {
    ShaderKeywordSet kw;
    vector<ShaderPassDesc> passes;
    passes.push_back(ShaderPassDesc{.PassTag = string{tag0}, .Source = ""});
    passes.push_back(ShaderPassDesc{.PassTag = string{tag1}, .Source = ""});
    return ShaderAsset{std::move(kw), std::move(passes)};
}

}  // namespace

TEST(RenderQueueTest, AddPrimitiveRejectsNulls) {
    DrawList list;
    FakeProxy proxy;
    ShaderAsset shader = MakeShader("ForwardLit");
    MaterialAsset material{&shader};

    EXPECT_FALSE(list.AddPrimitive(nullptr, &proxy, "ForwardLit"));
    EXPECT_FALSE(list.AddPrimitive(&material, nullptr, "ForwardLit"));
    EXPECT_TRUE(list.Empty());
}

TEST(RenderQueueTest, AddPrimitiveRejectsMaterialWithoutShader) {
    DrawList list;
    FakeProxy proxy;
    MaterialAsset material;  // no shader
    EXPECT_FALSE(list.AddPrimitive(&material, &proxy, "ForwardLit"));
    EXPECT_TRUE(list.Empty());
}

TEST(RenderQueueTest, PassTagFilteringDropsNonMatching) {
    DrawList list;
    FakeProxy proxy;
    ShaderAsset shader = MakeShader("ForwardLit");
    MaterialAsset material{&shader};

    // 匹配的 tag 加入。
    EXPECT_TRUE(list.AddPrimitive(&material, &proxy, "ForwardLit"));
    EXPECT_EQ(list.Size(), 1u);
    // 不匹配的 tag 丢弃。
    EXPECT_FALSE(list.AddPrimitive(&material, &proxy, "ShadowCaster"));
    EXPECT_EQ(list.Size(), 1u);
}

TEST(RenderQueueTest, PassIndexMatchesShaderPass) {
    DrawList list;
    FakeProxy proxy;
    ShaderAsset shader = MakeShaderTwoPasses("Depth", "ForwardLit");
    MaterialAsset material{&shader};

    ASSERT_TRUE(list.AddPrimitive(&material, &proxy, "ForwardLit"));
    ASSERT_EQ(list.Size(), 1u);
    EXPECT_EQ(list.Items()[0].PassIndex, 1u);  // ForwardLit 是第二个 pass

    list.Clear();
    ASSERT_TRUE(list.AddPrimitive(&material, &proxy, "Depth"));
    EXPECT_EQ(list.Items()[0].PassIndex, 0u);
}

TEST(RenderQueueTest, RenderQueueCopiedFromMaterial) {
    DrawList list;
    FakeProxy proxy;
    ShaderAsset shader = MakeShader("ForwardLit");
    MaterialAsset material{&shader};
    material.SetRenderQueue(RenderQueue::AlphaTest);

    ASSERT_TRUE(list.AddPrimitive(&material, &proxy, "ForwardLit"));
    EXPECT_EQ(list.Items()[0].RenderQueue, static_cast<int32_t>(RenderQueue::AlphaTest));
}

TEST(RenderQueueTest, SortOpaqueByQueueThenMaterialThenDistance) {
    DrawList list;
    FakeProxy proxy;
    ShaderAsset shader = MakeShader("ForwardLit");

    MaterialAsset geoA{&shader};
    geoA.SetRenderQueue(RenderQueue::Geometry);
    MaterialAsset geoB{&shader};
    geoB.SetRenderQueue(RenderQueue::Geometry);
    MaterialAsset bg{&shader};
    bg.SetRenderQueue(RenderQueue::Background);

    // 乱序加入。
    list.AddPrimitive(&geoA, &proxy, "ForwardLit", 0, 10.0f);
    list.AddPrimitive(&bg, &proxy, "ForwardLit", 0, 5.0f);
    list.AddPrimitive(&geoA, &proxy, "ForwardLit", 0, 2.0f);
    list.AddPrimitive(&geoB, &proxy, "ForwardLit", 0, 1.0f);

    list.SortOpaque();
    auto items = list.Items();
    ASSERT_EQ(items.size(), 4u);

    // Background (1000) 先于 Geometry (2000)。
    EXPECT_EQ(items[0].Material, &bg);
    // 剩下 3 个都是 Geometry: 按 material 指针聚合, 同 material 内近到远。
    // geoA 的两项 (dist 2.0, 10.0) 应相邻且升序; geoB 一项。
    // material 指针顺序不确定, 只验证 "同 material 聚合" 且 "同 material 内 dist 升序"。
    EXPECT_EQ(items[1].RenderQueue, static_cast<int32_t>(RenderQueue::Geometry));
    EXPECT_EQ(items[2].RenderQueue, static_cast<int32_t>(RenderQueue::Geometry));
    EXPECT_EQ(items[3].RenderQueue, static_cast<int32_t>(RenderQueue::Geometry));
    // 找到 geoA 的两项, 验证升序相邻。
    int firstGeoA = -1;
    for (int i = 1; i < 4; ++i) {
        if (items[i].Material == &geoA) {
            firstGeoA = i;
            break;
        }
    }
    ASSERT_NE(firstGeoA, -1);
    ASSERT_LT(firstGeoA + 1, 4);
    EXPECT_EQ(items[firstGeoA].Material, &geoA);
    EXPECT_EQ(items[firstGeoA + 1].Material, &geoA);
    EXPECT_LT(items[firstGeoA].ViewDistance, items[firstGeoA + 1].ViewDistance);
}

TEST(RenderQueueTest, SortTransparentBackToFront) {
    DrawList list;
    FakeProxy proxy;
    ShaderAsset shader = MakeShader("ForwardLit");

    MaterialAsset mat{&shader};
    mat.SetRenderQueue(RenderQueue::Transparent);

    list.AddPrimitive(&mat, &proxy, "ForwardLit", 0, 1.0f);
    list.AddPrimitive(&mat, &proxy, "ForwardLit", 0, 9.0f);
    list.AddPrimitive(&mat, &proxy, "ForwardLit", 0, 5.0f);

    list.SortTransparent();
    auto items = list.Items();
    ASSERT_EQ(items.size(), 3u);
    // 远到近: 9, 5, 1
    EXPECT_FLOAT_EQ(items[0].ViewDistance, 9.0f);
    EXPECT_FLOAT_EQ(items[1].ViewDistance, 5.0f);
    EXPECT_FLOAT_EQ(items[2].ViewDistance, 1.0f);
}

TEST(RenderQueueTest, SortTransparentQueueTakesPrecedenceOverDistance) {
    DrawList list;
    FakeProxy proxy;
    ShaderAsset shader = MakeShader("ForwardLit");

    MaterialAsset trans{&shader};
    trans.SetRenderQueue(RenderQueue::Transparent);  // 3000
    MaterialAsset overlay{&shader};
    overlay.SetRenderQueue(RenderQueue::Overlay);  // 4000

    // overlay 更近, 但队列更大 -> 排在 transparent 之后。
    list.AddPrimitive(&overlay, &proxy, "ForwardLit", 0, 0.5f);
    list.AddPrimitive(&trans, &proxy, "ForwardLit", 0, 100.0f);

    list.SortTransparent();
    auto items = list.Items();
    ASSERT_EQ(items.size(), 2u);
    EXPECT_EQ(items[0].Material, &trans);
    EXPECT_EQ(items[1].Material, &overlay);
}

TEST(RenderQueueTest, ClearEmptiesList) {
    DrawList list;
    FakeProxy proxy;
    ShaderAsset shader = MakeShader("ForwardLit");
    MaterialAsset material{&shader};
    list.AddPrimitive(&material, &proxy, "ForwardLit");
    EXPECT_FALSE(list.Empty());
    list.Clear();
    EXPECT_TRUE(list.Empty());
    EXPECT_EQ(list.Size(), 0u);
}

TEST(MaterialAssetTest, IsTransparentThreshold) {
    ShaderAsset shader = MakeShader("ForwardLit");
    MaterialAsset m{&shader};
    m.SetRenderQueue(RenderQueue::Geometry);
    EXPECT_FALSE(m.IsTransparent());
    m.SetRenderQueue(RenderQueue::GeometryLast);
    EXPECT_FALSE(m.IsTransparent());
    m.SetRenderQueue(RenderQueue::Transparent);
    EXPECT_TRUE(m.IsTransparent());
    m.SetRenderQueue(RenderQueue::Overlay);
    EXPECT_TRUE(m.IsTransparent());
    m.SetRenderQueue(2999);
    EXPECT_FALSE(m.IsTransparent());
    m.SetRenderQueue(3000);
    EXPECT_TRUE(m.IsTransparent());
}
