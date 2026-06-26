#include <gtest/gtest.h>

#include <radray/runtime/render/renderer_list.h>
#include <radray/runtime/render/renderer.h>
#include <radray/runtime/render/material.h>
#include <radray/runtime/render/shader.h>

using namespace radray;
using namespace radray::srp;

namespace {

class FakeMaterial : public Material {
public:
    FakeMaterial(BlendMode mode) : _mode(mode) {}
    Shader* GetShader() const override { return nullptr; }
    BlendMode GetBlendMode() const override { return _mode; }
    render::DescriptorSet* GetDescriptorSet(render::RootSignature*) const override { return nullptr; }

private:
    BlendMode _mode;
};

class FakeRenderer : public Renderer {
public:
    FakeRenderer(Material* m, uint32_t layer, bool visible) : _mat(m), _layer(layer), _visible(visible) {}
    MeshBatchElement BatchElement() const override { return {}; }
    const render::VertexBufferLayout& GetVertexLayout() const override {
        static render::VertexBufferLayout kEmpty{};
        return kEmpty;
    }
    const Eigen::Matrix4f& WorldMatrix() const override { return _world; }
    uint32_t LayerMask() const override { return _layer; }
    bool IsVisible() const override { return _visible; }
    Material* GetMaterial() const override { return _mat; }

private:
    Material* _mat;
    uint32_t _layer;
    bool _visible;
    Eigen::Matrix4f _world{Eigen::Matrix4f::Identity()};
};

}  // namespace

TEST(SrpFilterTest, QueueRangeSeparatesOpaqueTransparent) {
    FakeMaterial opaque{BlendMode::Opaque};
    FakeMaterial masked{BlendMode::Masked};
    FakeMaterial transp{BlendMode::Transparent};
    FakeRenderer rOpaque{&opaque, 0xFFFFFFFFu, true};
    FakeRenderer rMasked{&masked, 0xFFFFFFFFu, true};
    FakeRenderer rTransp{&transp, 0xFFFFFFFFu, true};

    FilteringSettings opaqueFilter{.QueueRange = RenderQueueRange::Opaque()};
    EXPECT_TRUE(opaqueFilter.Test(rOpaque));
    EXPECT_TRUE(opaqueFilter.Test(rMasked));   // masked 算不透明
    EXPECT_FALSE(opaqueFilter.Test(rTransp));

    FilteringSettings transparentFilter{.QueueRange = RenderQueueRange::Transparent()};
    EXPECT_FALSE(transparentFilter.Test(rOpaque));
    EXPECT_FALSE(transparentFilter.Test(rMasked));
    EXPECT_TRUE(transparentFilter.Test(rTransp));
}

TEST(SrpFilterTest, LayerMaskAndVisibility) {
    FakeMaterial opaque{BlendMode::Opaque};
    FakeRenderer onLayer1{&opaque, 0x1, true};
    FakeRenderer onLayer2{&opaque, 0x2, true};
    FakeRenderer invisible{&opaque, 0xFFFFFFFFu, false};

    FilteringSettings layer1{.QueueRange = RenderQueueRange::All(), .LayerMask = 0x1};
    EXPECT_TRUE(layer1.Test(onLayer1));
    EXPECT_FALSE(layer1.Test(onLayer2));  // layer 不匹配

    FilteringSettings all{.QueueRange = RenderQueueRange::All()};
    EXPECT_FALSE(all.Test(invisible));  // 不可见
}

TEST(SrpFilterTest, NullMaterialRejected) {
    FakeRenderer noMat{nullptr, 0xFFFFFFFFu, true};
    FilteringSettings all{.QueueRange = RenderQueueRange::All()};
    EXPECT_FALSE(all.Test(noMat));
}
