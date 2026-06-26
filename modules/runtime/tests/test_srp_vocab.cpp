#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include <radray/runtime/render/render_pass_event.h>
#include <radray/runtime/render/tag_set.h>
#include <radray/runtime/render/keyword_set.h>
#include <radray/runtime/render/per_object_data.h>
#include <radray/runtime/render/sorting.h>
#include <radray/runtime/render/scene_view.h>

using namespace radray;
using namespace radray::srp;

TEST(SrpVocabTest, RenderPassEventSparseOrdering) {
    // 稀疏数值,留插入间隙;阴影 < 不透明 < 透明。
    EXPECT_LT(static_cast<int32_t>(RenderPassEvent::BeforeRenderingShadows),
              static_cast<int32_t>(RenderPassEvent::BeforeRenderingOpaques));
    EXPECT_LT(static_cast<int32_t>(RenderPassEvent::BeforeRenderingOpaques),
              static_cast<int32_t>(RenderPassEvent::BeforeRenderingTransparents));
    // 间隙足够插入自定义事件。
    EXPECT_GT(static_cast<int32_t>(RenderPassEvent::AfterRenderingOpaques) -
                  static_cast<int32_t>(RenderPassEvent::BeforeRenderingOpaques),
              1);
}

TEST(SrpVocabTest, TagSetFindAndLightMode) {
    TagSet tags{.Tags = {{"LightMode", "UniversalForward"}, {"Queue", "Geometry"}}};
    auto lm = tags.LightMode();
    ASSERT_TRUE(lm.has_value());
    EXPECT_EQ(*lm, "UniversalForward");
    auto q = tags.Find("Queue");
    ASSERT_TRUE(q.has_value());
    EXPECT_EQ(*q, "Geometry");
    EXPECT_FALSE(tags.Find("Missing").has_value());
}

TEST(SrpVocabTest, KeywordSetMergeIsOrderIndependent) {
    KeywordSet a;
    a.Add("_NORMALMAP");
    a.Add("SHADOWS_ON");
    KeywordSet b;
    b.Add("SHADOWS_ON");
    b.Add("_NORMALMAP");
    EXPECT_EQ(a, b);
    EXPECT_EQ(ShaderVariantKeyHash{}(a), ShaderVariantKeyHash{}(b));
}

TEST(SrpVocabTest, PerObjectDataFlags) {
    PerObjectDataFlags forward = PerObjectData::LightProbe | PerObjectData::LightData;
    EXPECT_TRUE(forward.HasFlag(PerObjectData::LightProbe));
    EXPECT_TRUE(forward.HasFlag(PerObjectData::LightData));
    EXPECT_FALSE(forward.HasFlag(PerObjectData::MotionVectors));

    PerObjectDataFlags none = PerObjectData::None;
    EXPECT_FALSE(static_cast<bool>(none));
}

TEST(SrpVocabTest, RenderQueueRangeSplit) {
    auto opaque = RenderQueueRange::Opaque();
    auto transp = RenderQueueRange::Transparent();
    EXPECT_TRUE(opaque.Contains(RenderQueue::Opaque));
    EXPECT_TRUE(opaque.Contains(RenderQueue::AlphaTest));
    EXPECT_FALSE(opaque.Contains(RenderQueue::Transparent));
    EXPECT_TRUE(transp.Contains(RenderQueue::Transparent));
    EXPECT_FALSE(transp.Contains(RenderQueue::AlphaTest));
}

TEST(SrpVocabTest, SceneViewDefaultsIdentity) {
    SceneView v;
    EXPECT_TRUE(v.ViewMatrix.isIdentity());
    EXPECT_TRUE(v.ProjMatrix.isIdentity());
    EXPECT_EQ(v.ViewportWidth, 0u);
}
