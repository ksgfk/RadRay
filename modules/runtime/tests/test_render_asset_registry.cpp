#include <array>

#include <gtest/gtest.h>

#include <radray/triangle_mesh.h>
#include <radray/runtime/render_asset_registry.h>

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
    resource.Name = "runtime_registry_quad";
    return resource;
}

}  // namespace

TEST(RenderHandlesTest, DefaultHandlesAreInvalid) {
    EXPECT_FALSE(MeshHandle{}.IsValid());
    EXPECT_FALSE(MaterialHandle{}.IsValid());
    EXPECT_FALSE(TextureHandle{}.IsValid());
    EXPECT_FALSE(SamplerHandle{}.IsValid());
    EXPECT_LT(MeshHandle{1}.Value, MeshHandle{2}.Value);
}

class RenderAssetRegistryDeviceTest : public ::testing::TestWithParam<runtime::test::TestBackend> {
};

TEST_P(RenderAssetRegistryDeviceTest, RegistersAndResolvesAssets) {
    runtime::test::RuntimeTestContext ctx{};
    string reason{};
    if (!ctx.Initialize(GetParam(), &reason)) {
        GTEST_SKIP() << reason;
    }

    RenderAssetRegistry registry{ctx.GetDevicePtr()};
    const MeshResource meshResource = CreateQuadMeshResource();

    const MeshHandle meshHandle = registry.RegisterMesh(meshResource);
    ASSERT_TRUE(meshHandle.IsValid());
    ASSERT_TRUE(registry.ResolveMesh(meshHandle).HasValue());
    ASSERT_FALSE(registry.ResolveMesh(MeshHandle{}).HasValue());

    TextureInitDesc textureDesc{};
    textureDesc.DebugName = "runtime_registry_texture";
    textureDesc.Desc.Dim = render::TextureDimension::Dim2D;
    textureDesc.Desc.Width = 2;
    textureDesc.Desc.Height = 2;
    textureDesc.Desc.DepthOrArraySize = 1;
    textureDesc.Desc.MipLevels = 1;
    textureDesc.Desc.SampleCount = 1;
    textureDesc.Desc.Format = render::TextureFormat::RGBA8_UNORM;
    textureDesc.Desc.Memory = render::MemoryType::Device;
    textureDesc.Desc.Usage = render::TextureUse::Resource;
    const vector<byte> textureBytes = CreateSolidTextureBytes(2, 2, {255, 64, 32, 255});

    const TextureHandle textureHandle = registry.RegisterTexture2D(textureDesc, textureBytes);
    ASSERT_TRUE(textureHandle.IsValid());
    auto texture = registry.ResolveTexture(textureHandle);
    ASSERT_TRUE(texture.HasValue());
    EXPECT_TRUE(static_cast<bool>(texture.Get()->Desc.Usage & render::TextureUse::Resource));
    EXPECT_TRUE(static_cast<bool>(texture.Get()->Desc.Usage & render::TextureUse::CopyDestination));

    const SamplerHandle samplerHandle = registry.RegisterSampler(CreateLinearClampSamplerDesc());
    ASSERT_TRUE(samplerHandle.IsValid());
    ASSERT_TRUE(registry.ResolveSampler(samplerHandle).HasValue());

    const MaterialHandle invalidMaterial = registry.RegisterForwardOpaqueMaterial(ForwardOpaqueMaterialDesc{
        .Albedo = TextureHandle{999},
        .Sampler = samplerHandle,
    });
    EXPECT_FALSE(invalidMaterial.IsValid());
    ctx.ClearCapturedErrors();

    const MaterialHandle materialHandle = registry.RegisterForwardOpaqueMaterial(ForwardOpaqueMaterialDesc{
        .Albedo = textureHandle,
        .Sampler = samplerHandle,
        .Tint = Eigen::Vector4f{0.5f, 1.0f, 0.75f, 1.0f},
        .DebugName = "runtime_registry_material",
    });
    ASSERT_TRUE(materialHandle.IsValid());
    auto material = registry.ResolveMaterial(materialHandle);
    ASSERT_TRUE(material.HasValue());
    EXPECT_EQ(material.Get()->Desc.Albedo.Value, textureHandle.Value);
    EXPECT_EQ(material.Get()->Desc.Sampler.Value, samplerHandle.Value);

    const auto pendingBefore = registry.GetPendingUploads();
    ASSERT_EQ(pendingBefore.size(), 2u);
    EXPECT_EQ(pendingBefore[0].UploadKind, PendingUploadHandle::Kind::Mesh);
    EXPECT_EQ(pendingBefore[1].UploadKind, PendingUploadHandle::Kind::Texture);

    registry.RemovePendingUpload(pendingBefore[0]);
    const auto pendingAfterMesh = registry.GetPendingUploads();
    ASSERT_EQ(pendingAfterMesh.size(), 1u);
    EXPECT_EQ(pendingAfterMesh[0].UploadKind, PendingUploadHandle::Kind::Texture);

    registry.RemovePendingUpload(pendingAfterMesh[0]);
    EXPECT_TRUE(registry.GetPendingUploads().empty());

    const auto errors = ctx.GetCapturedErrors();
    EXPECT_TRUE(errors.empty()) << ctx.JoinCapturedErrors();
}

INSTANTIATE_TEST_SUITE_P(
    DeviceBackends,
    RenderAssetRegistryDeviceTest,
    ::testing::ValuesIn(runtime::test::GetEnabledDeviceBackends()));
