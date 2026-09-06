#include "graph_compile_device.h"
#include <gtest/gtest.h>
#include <radray/runtime/render_framework/render_graph.h>

namespace radray {
namespace {
render::TextureDescriptor TextureDesc() {
    return {render::TextureDimension::Dim2D, 32, 16, 2, 3, 1, render::TextureFormat::RGBA8_UNORM,
            render::MemoryType::Device, render::TextureUse::CopyDestination | render::TextureUse::Resource};
}
}  // namespace
TEST(TextureRegionTest, ValidatesPitchBoundsMipLayerUsageAndOverflow) {
    render::DeviceDetail detail;
    detail.TextureDataPlacementAlignment = 512;
    detail.TextureDataPitchAlignment = 256;
    const render::BufferDescriptor buffer{2048, render::MemoryType::Upload, render::BufferUse::CopySource | render::BufferUse::MapWrite};
    const auto texture = TextureDesc();
    render::BufferTextureCopyRegion region{512, 256, 1, 1, 3, 2, 4, 3};
    const auto valid = [&](const render::BufferTextureCopyRegion& r, const render::TextureDescriptor& t = TextureDesc()) { return render::ValidateBufferTextureCopyRegion(buffer, t, r, detail).Supported; };
    EXPECT_TRUE(valid(region));
    auto invalid = region;
    invalid.SourceOffset = 511;
    EXPECT_FALSE(valid(invalid));
    invalid = region;
    invalid.RowPitch = 255;
    EXPECT_FALSE(valid(invalid));
    invalid = region;
    invalid.Width = 14;
    EXPECT_FALSE(valid(invalid));
    invalid = region;
    invalid.ArrayLayer = 2;
    EXPECT_FALSE(valid(invalid));
    invalid = region;
    invalid.MipLevel = 3;
    EXPECT_FALSE(valid(invalid));
    invalid = region;
    invalid.MipLevel = 32;
    auto invalidMips = texture;
    invalidMips.MipLevels = 33;
    EXPECT_FALSE(valid(invalid, invalidMips));
    invalid = region;
    invalid.SourceOffset = UINT64_MAX - 511;
    EXPECT_FALSE(valid(invalid));
    invalid = region;
    invalid.Height = 0;
    EXPECT_FALSE(valid(invalid));
    auto msaa = texture;
    msaa.SampleCount = 4;
    EXPECT_FALSE(valid(region, msaa));
    auto usage = texture;
    usage.Usage = render::TextureUse::Resource;
    EXPECT_FALSE(valid(region, usage));
}
TEST(TextureRegionTest, GraphRejectsPartialInitializationAndCrossGraphHandlesBeforeAllocation) {
    test::GraphCompileDevice device;
    render::RenderPassRegistry registry(&device);
    RenderResourcePool pool(device, registry);
    pool.BeginFlight(1);
    RenderGraph graph(device, pool, registry, "partial upload");
    const auto texture = graph.CreateTexture(TextureDesc(), "destination");
    const auto buffer = graph.CreateBuffer({4096, render::MemoryType::Device, render::BufferUse::UnorderedAccess | render::BufferUse::CopySource}, "source");
    graph.AddComputePass<int>("initialize source", [&](int&, RenderGraphComputeBuilder& builder) { builder.WriteBuffer(buffer); }, nullptr);
    graph.AddCopyBufferToTexturePass("partial", buffer, texture, {0, 128, 1, 1, 0, 0, 4, 4});
    graph.AddComputePass<int>("consume", [&](int&, RenderGraphComputeBuilder& builder) { builder.ReadTexture(texture, {.Range = {1, 1, 1, 1}}); builder.SetSideEffect(); }, nullptr);
    EXPECT_FALSE(graph.Compile());
    EXPECT_EQ(device.NativeCreates, 0u);
    RenderGraph other(device, pool, registry, "other graph");
    EXPECT_FALSE(other.GetTextureDescriptor(texture));
}
}  // namespace radray
