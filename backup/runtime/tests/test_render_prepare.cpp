#include <array>

#include <gtest/gtest.h>

#include <radray/triangle_mesh.h>
#include <radray/runtime/frame_snapshot.h>
#include <radray/runtime/render_prepare.h>

#include "runtime_test_framework.h"

using namespace radray;
using namespace radray::runtime;

namespace {

render::SamplerDescriptor CreateLinearClampSamplerDesc() {
    render::SamplerDescriptor desc{};
    desc.AddressS = render::AddressMode::ClampToEdge;
    desc.AddressT = render::AddressMode::ClampToEdge;
    desc.AddressR = render::AddressMode::ClampToEdge;
    desc.MinFilter = render::FilterMode::Linear;
    desc.MagFilter = render::FilterMode::Linear;
    desc.MipmapFilter = render::FilterMode::Linear;
    desc.LodMin = 0.0f;
    desc.LodMax = 0.0f;
    desc.AnisotropyClamp = 1;
    return desc;
}

vector<byte> CreateSolidTextureBytes(uint32_t width, uint32_t height, std::array<uint8_t, 4> rgba) {
    vector<byte> texels(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < texels.size(); i += 4) {
        texels[i + 0] = static_cast<byte>(rgba[0]);
        texels[i + 1] = static_cast<byte>(rgba[1]);
        texels[i + 2] = static_cast<byte>(rgba[2]);
        texels[i + 3] = static_cast<byte>(rgba[3]);
    }
    return texels;
}

MeshResource CreateQuadMeshResource() {
    TriangleMesh mesh{};
    mesh.InitAsRectXY(2.0f, 2.0f);
    MeshResource resource{};
    mesh.ToSimpleMeshResource(&resource);
    resource.Name = "runtime_prepare_quad";
    return resource;
}

}  // namespace

class RenderPrepareDeviceTest : public ::testing::TestWithParam<runtime::test::TestBackend> {
};

TEST_P(RenderPrepareDeviceTest, RoutesBatchesToMatchingViewsAndValidatesSubmeshes) {
    runtime::test::RuntimeTestContext ctx{};
    string reason{};
    if (!ctx.Initialize(GetParam(), &reason)) {
        GTEST_SKIP() << reason;
    }

    RenderAssetRegistry registry{ctx.GetDevicePtr()};
    const MeshHandle meshHandle = registry.RegisterMesh(CreateQuadMeshResource());
    ASSERT_TRUE(meshHandle.IsValid());

    TextureInitDesc textureDesc{};
    textureDesc.DebugName = "runtime_prepare_texture";
    textureDesc.Desc.Dim = render::TextureDimension::Dim2D;
    textureDesc.Desc.Width = 2;
    textureDesc.Desc.Height = 2;
    textureDesc.Desc.DepthOrArraySize = 1;
    textureDesc.Desc.MipLevels = 1;
    textureDesc.Desc.SampleCount = 1;
    textureDesc.Desc.Format = render::TextureFormat::RGBA8_UNORM;
    textureDesc.Desc.Memory = render::MemoryType::Device;
    textureDesc.Desc.Usage = render::TextureUse::Resource;
    const TextureHandle textureHandle = registry.RegisterTexture2D(
        textureDesc,
        CreateSolidTextureBytes(2, 2, {255, 255, 255, 255}));
    ASSERT_TRUE(textureHandle.IsValid());
    const SamplerHandle samplerHandle = registry.RegisterSampler(CreateLinearClampSamplerDesc());
    ASSERT_TRUE(samplerHandle.IsValid());
    const MaterialHandle materialHandle = registry.RegisterForwardOpaqueMaterial(ForwardOpaqueMaterialDesc{
        .Albedo = textureHandle,
        .Sampler = samplerHandle,
    });
    ASSERT_TRUE(materialHandle.IsValid());

    FrameSnapshotBuilder builder{};
    builder.Reset(1, 1);
    builder.AddView() = RenderViewRequest{.ViewId = 0, .CameraId = 10, .OutputWidth = 64, .OutputHeight = 64};
    builder.AddView() = RenderViewRequest{.ViewId = 1, .CameraId = 11, .OutputWidth = 64, .OutputHeight = 64};
    builder.AddCamera() = CameraRenderData{.CameraId = 10, .ViewId = 0, .OutputWidth = 64, .OutputHeight = 64};
    builder.AddCamera() = CameraRenderData{.CameraId = 11, .ViewId = 1, .OutputWidth = 64, .OutputHeight = 64};
    builder.AddMeshBatch() = VisibleMeshBatch{
        .ViewMask = 1u << 1,
        .Mesh = meshHandle,
        .Material = materialHandle,
        .SubmeshIndex = 0,
        .SortKeyHigh = 1,
    };
    FrameSnapshot snapshot = builder.Finalize(&reason);
    ASSERT_TRUE(reason.empty()) << reason;

    PreparedScene prepared{};
    ASSERT_TRUE(PrepareScene(snapshot, registry, prepared, &reason)) << reason;
    ASSERT_EQ(prepared.Views.size(), 2u);
    EXPECT_TRUE(prepared.Views[0].DrawItems.empty());
    ASSERT_EQ(prepared.Views[1].DrawItems.size(), 1u);
    EXPECT_EQ(prepared.Views[1].DrawItems[0].SubmeshIndex, 0u);

    builder.Reset(2, 2);
    builder.AddView() = RenderViewRequest{.ViewId = 0, .CameraId = 10, .OutputWidth = 64, .OutputHeight = 64};
    builder.AddCamera() = CameraRenderData{.CameraId = 10, .ViewId = 0, .OutputWidth = 64, .OutputHeight = 64};
    builder.AddMeshBatch() = VisibleMeshBatch{
        .ViewMask = 0,
        .Mesh = meshHandle,
        .Material = materialHandle,
        .SubmeshIndex = 99,
    };
    snapshot = builder.Finalize(&reason);
    ASSERT_TRUE(reason.empty()) << reason;
    EXPECT_FALSE(PrepareScene(snapshot, registry, prepared, &reason));
    EXPECT_FALSE(reason.empty());
}

INSTANTIATE_TEST_SUITE_P(
    DeviceBackends,
    RenderPrepareDeviceTest,
    ::testing::ValuesIn(runtime::test::GetEnabledDeviceBackends()));
