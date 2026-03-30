#include <string_view>

#include <gtest/gtest.h>

#include <radray/render/common.h>
#include <radray/runtime/render_graph.h>

using namespace radray;
using namespace radray::render;

namespace {

GpuBufferHandle MakeBufferHandle(uint64_t handleValue) {
    GpuBufferHandle handle{};
    handle.Handle = handleValue;
    return handle;
}

GpuTextureHandle MakeTextureHandle(uint64_t handleValue) {
    GpuTextureHandle handle{};
    handle.Handle = handleValue;
    return handle;
}

void ExpectContains(const string& actual, std::string_view expected) {
    EXPECT_NE(actual.find(expected), string::npos) << "missing substring: " << expected << "\nDOT:\n" << actual;
}

void ExpectNotContains(const string& actual, std::string_view expected) {
    EXPECT_EQ(actual.find(expected), string::npos) << "unexpected substring: " << expected << "\nDOT:\n" << actual;
}

bool IsBufferRangeEqual(const BufferRange& lhs, const BufferRange& rhs) {
    return lhs.Offset == rhs.Offset && lhs.Size == rhs.Size;
}

bool IsSubresourceRangeEqual(const SubresourceRange& lhs, const SubresourceRange& rhs) {
    return lhs.BaseArrayLayer == rhs.BaseArrayLayer &&
        lhs.ArrayLayerCount == rhs.ArrayLayerCount &&
        lhs.BaseMipLevel == rhs.BaseMipLevel &&
        lhs.MipLevelCount == rhs.MipLevelCount;
}

const RDGEdge* FindBufferEdge(
    const RenderGraph& graph,
    RDGNodeHandle from,
    RDGNodeHandle to,
    RDGExecutionStage stage,
    RDGMemoryAccess access,
    BufferRange range) {
    for (const auto& edge : graph._edges) {
        if (edge == nullptr ||
            edge->_from == nullptr ||
            edge->_to == nullptr ||
            edge->_from->_id != from.Id ||
            edge->_to->_id != to.Id ||
            edge->_stage != stage ||
            edge->_access != access) {
            continue;
        }
        if (IsBufferRangeEqual(edge->_bufferRange, range)) {
            return edge.get();
        }
    }
    return nullptr;
}

const RDGEdge* FindTextureEdge(
    const RenderGraph& graph,
    RDGNodeHandle from,
    RDGNodeHandle to,
    RDGExecutionStage stage,
    RDGMemoryAccess access,
    RDGTextureLayout layout,
    SubresourceRange range) {
    for (const auto& edge : graph._edges) {
        if (edge == nullptr ||
            edge->_from == nullptr ||
            edge->_to == nullptr ||
            edge->_from->_id != from.Id ||
            edge->_to->_id != to.Id ||
            edge->_stage != stage ||
            edge->_access != access ||
            edge->_textureLayout != layout) {
            continue;
        }
        if (IsSubresourceRangeEqual(edge->_textureRange, range)) {
            return edge.get();
        }
    }
    return nullptr;
}

TEST(RenderGraphTest, ExportGraphviz_ComplexDependencyGraphProducesCompactDot) {
    RenderGraph graph{};

    const auto sceneBuffer = graph.ImportBuffer(
        MakeBufferHandle(1),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        BufferRange{0, 512},
        "SceneCB");
    const auto lightListBuffer = graph.AddBuffer(8192, "LightListBuffer");
    const auto readbackBuffer = graph.AddBuffer(16384, "ReadbackBuffer");

    const auto blueNoiseTexture = graph.ImportTexture(
        MakeTextureHandle(11),
        RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        SubresourceRange{0, 1, 0, 1},
        "Blue \"Noise\"\\Cache");
    const auto gbufferColor = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        "GBufferColor");
    const auto depthTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::D32_FLOAT,
        "SceneDepth");
    const auto lightingTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        "LightingTarget");
    const auto presentTexture = graph.ImportTexture(
        MakeTextureHandle(12),
        RDGExecutionStage::Present,
        RDGMemoryAccess::NONE,
        RDGTextureLayout::Present,
        SubresourceRange{0, 1, 0, 1},
        "Swapchain\nOutput(Target)");

    const auto uploadPass = graph.AddPass("UploadPass");
    const auto gbufferPass = graph.AddPass("GBufferPass");
    const auto lightCullingPass = graph.AddPass("LightCullingPass");
    const auto lightingPass = graph.AddPass("LightingPass");
    const auto postFxPass = graph.AddPass("PostFXPass");
    const auto copyReadbackPass = graph.AddPass("CopyReadbackPass");

    static_cast<RDGPassNode*>(graph._nodes[uploadPass.Id].get())->_type = QueueType::Copy;
    static_cast<RDGPassNode*>(graph._nodes[lightCullingPass.Id].get())->_type = QueueType::Compute;
    static_cast<RDGPassNode*>(graph._nodes[copyReadbackPass.Id].get())->_type = QueueType::Copy;

    graph.Link(sceneBuffer, uploadPass, RDGExecutionStage::Host, RDGMemoryAccess::HostWrite, BufferRange{0, 512});
    graph.Link(uploadPass, lightListBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, BufferRange{128, 2048});

    graph.Link(sceneBuffer, gbufferPass, RDGExecutionStage::VertexShader, RDGMemoryAccess::ConstantRead, BufferRange{256, 128});
    graph.Link(
        blueNoiseTexture,
        gbufferPass,
        RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        SubresourceRange{0, 1, 0, 1});
    graph.Link(
        gbufferPass,
        gbufferColor,
        RDGExecutionStage::ColorOutput,
        RDGMemoryAccess::ColorAttachmentWrite,
        RDGTextureLayout::ColorAttachment,
        SubresourceRange{0, 1, 0, 1});
    graph.Link(
        gbufferPass,
        depthTexture,
        RDGExecutionStage::DepthStencil,
        RDGMemoryAccess::DepthStencilWrite,
        RDGTextureLayout::DepthStencilAttachment,
        SubresourceRange{0, 1, 0, 1});

    graph.Link(lightListBuffer, lightCullingPass, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, BufferRange{128, 2048});
    graph.Link(
        gbufferColor,
        lightCullingPass,
        RDGExecutionStage::ComputeShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        SubresourceRange{0, 1, 0, 1});
    graph.Link(
        depthTexture,
        lightCullingPass,
        RDGExecutionStage::ComputeShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::DepthStencilReadOnly,
        SubresourceRange{0, 1, 0, 1});
    graph.Link(lightCullingPass, lightListBuffer, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderWrite, BufferRange{4096, 2048});

    graph.Link(lightListBuffer, lightingPass, RDGExecutionStage::PixelShader, RDGMemoryAccess::ShaderRead, BufferRange{4096, 2048});
    graph.Link(
        gbufferColor,
        lightingPass,
        RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        SubresourceRange{0, 1, 0, 1});
    graph.Link(
        depthTexture,
        lightingPass,
        RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::DepthStencilReadOnly,
        SubresourceRange{0, 1, 0, 1});
    graph.Link(
        blueNoiseTexture,
        lightingPass,
        RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        SubresourceRange{0, 1, 0, 1});
    graph.Link(
        lightingPass,
        lightingTexture,
        RDGExecutionStage::ColorOutput,
        RDGMemoryAccess::ColorAttachmentWrite,
        RDGTextureLayout::ColorAttachment,
        SubresourceRange{0, 1, 0, 1});

    graph.Link(
        lightingTexture,
        postFxPass,
        RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        SubresourceRange{0, 1, 0, 1});
    graph.Link(
        postFxPass,
        presentTexture,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        RDGTextureLayout::TransferDestination,
        SubresourceRange{0, 1, 0, 1});

    graph.Link(
        lightingTexture,
        copyReadbackPass,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        SubresourceRange{0, 1, 0, 1});
    graph.Link(copyReadbackPass, readbackBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, BufferRange{0, 16384});

    graph.ExportBuffer(readbackBuffer, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, BufferRange{0, 16384});
    graph.ExportTexture(
        presentTexture,
        RDGExecutionStage::Present,
        RDGMemoryAccess::NONE,
        RDGTextureLayout::Present,
        SubresourceRange{0, 1, 0, 1});

    const auto validation = graph.Validate();
    ASSERT_TRUE(validation.first) << validation.second;

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "digraph RenderGraph");
    ExpectContains(dot, "rankdir=LR");
    ExpectContains(dot, "shape=box");
    ExpectContains(dot, "shape=ellipse");
    ExpectContains(dot, "shape=hexagon");

    ExpectContains(dot, "UploadPass");
    ExpectContains(dot, "GBufferPass");
    ExpectContains(dot, "LightCullingPass");
    ExpectContains(dot, "LightingPass");
    ExpectContains(dot, "PostFXPass");
    ExpectContains(dot, "CopyReadbackPass");
    ExpectContains(dot, "SceneCB");
    ExpectContains(dot, "LightListBuffer");
    ExpectContains(dot, "ReadbackBuffer");
    ExpectContains(dot, "GBufferColor");
    ExpectContains(dot, "SceneDepth");
    ExpectContains(dot, "LightingTarget");
    ExpectContains(dot, "Blue \\\"Noise\\\"\\\\Cache");
    ExpectContains(dot, "Swapchain\\nOutput(Target)");

    ExpectContains(dot, "queue=Copy");
    ExpectContains(dot, "queue=Compute");
    ExpectContains(dot, "queue=Direct");

    ExpectContains(dot, "[VertexShader]");
    ExpectContains(dot, "[PixelShader]");
    ExpectContains(dot, "[ComputeShader]");
    ExpectContains(dot, "[ColorOutput]");
    ExpectContains(dot, "[DepthStencil]");
    ExpectContains(dot, "[Copy]");
    ExpectContains(dot, "[ConstantRead]");
    ExpectContains(dot, "[ShaderRead]");
    ExpectContains(dot, "[ShaderWrite]");
    ExpectContains(dot, "[TransferRead]");
    ExpectContains(dot, "[TransferWrite]");
    ExpectContains(dot, "[ColorAttachmentWrite]");
    ExpectContains(dot, "[DepthStencilWrite]");

    ExpectNotContains(dot, "type=Pass");
    ExpectNotContains(dot, "ownership=External");
    ExpectNotContains(dot, "dim=2D");
    ExpectNotContains(dot, "format=RGBA16_FLOAT");
    ExpectNotContains(dot, "imported=");
    ExpectNotContains(dot, "exported=");
    ExpectNotContains(dot, "layout=");
    ExpectNotContains(dot, "range={");

    ExpectContains(dot, string("node_") + std::to_string(uploadPass.Id));
    ExpectContains(dot, string("node_") + std::to_string(presentTexture.Id));
    ExpectContains(dot, string("node_") + std::to_string(sceneBuffer.Id) + " -> node_" + std::to_string(uploadPass.Id));
    ExpectContains(dot, string("node_") + std::to_string(gbufferPass.Id) + " -> node_" + std::to_string(gbufferColor.Id));
    ExpectContains(dot, string("node_") + std::to_string(lightingTexture.Id) + " -> node_" + std::to_string(copyReadbackPass.Id));
    ExpectContains(dot, string("node_") + std::to_string(postFxPass.Id) + " -> node_" + std::to_string(presentTexture.Id));
}

TEST(RenderGraphTest, RasterPassBuilderBuildsEdgesAndStoresAttachmentMetadata) {
    RenderGraph graph{};

    const auto vertexBuffer = graph.AddBuffer(4096, "VertexBuffer");
    const auto sceneCBuffer = graph.AddBuffer(256, "SceneCB");
    const auto storageBuffer = graph.AddBuffer(1024, "StorageBuffer");
    const auto lightingTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        1280,
        720,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        "LightingTarget");
    const auto depthTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        1280,
        720,
        1,
        1,
        1,
        TextureFormat::D32_FLOAT,
        "SceneDepth");
    const auto blueNoiseTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        128,
        128,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        "BlueNoise");
    const auto historyTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        1280,
        720,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        "HistoryTexture");

    RDGRasterPassBuilder builder{&graph};
    const auto colorRange = SubresourceRange::AllSub();
    const auto depthRange = SubresourceRange{0, 1, 0, 1};
    const auto colorClear = ColorClearValue{{0.1f, 0.2f, 0.3f, 1.0f}};
    const auto pass = builder
        .UseColorAttachment(0, lightingTexture, colorRange, LoadAction::Clear, StoreAction::Store, colorClear)
        .UseDepthStencilAttachment(depthTexture, depthRange, LoadAction::Load, StoreAction::Discard, LoadAction::Load, StoreAction::Discard, std::nullopt)
        .UseVertexBuffer(vertexBuffer, BufferRange{0, 1024})
        .UseCBuffer(sceneCBuffer, ShaderStage::Graphics, BufferRange{0, 256})
        .UseTexture(blueNoiseTexture, ShaderStage::Pixel, colorRange)
        .UseRWBuffer(storageBuffer, ShaderStage::Pixel, BufferRange{128, 256})
        .UseRWTexture(historyTexture, ShaderStage::Pixel, colorRange)
        .Build();

    EXPECT_EQ(builder.Build().Id, pass.Id);

    const auto validation = graph.Validate();
    ASSERT_TRUE(validation.first) << validation.second;

    auto* passNode = static_cast<RDGGraphicsPassNode*>(graph._nodes[pass.Id].get());
    ASSERT_NE(passNode, nullptr);
    EXPECT_EQ(passNode->_type, QueueType::Direct);
    ASSERT_EQ(passNode->_colorAttachments.size(), 1u);
    EXPECT_EQ(passNode->_colorAttachments[0].Slot, 0u);
    EXPECT_EQ(passNode->_colorAttachments[0].Texture.Id, lightingTexture.Id);
    EXPECT_TRUE(IsSubresourceRangeEqual(passNode->_colorAttachments[0].Range, colorRange));
    ASSERT_TRUE(passNode->_colorAttachments[0].ClearValue.has_value());
    EXPECT_FLOAT_EQ(passNode->_colorAttachments[0].ClearValue->Value[0], colorClear.Value[0]);
    EXPECT_FLOAT_EQ(passNode->_colorAttachments[0].ClearValue->Value[3], colorClear.Value[3]);
    ASSERT_TRUE(passNode->_depthStencilAttachment.has_value());
    EXPECT_EQ(passNode->_depthStencilAttachment->Texture.Id, depthTexture.Id);
    EXPECT_TRUE(IsSubresourceRangeEqual(passNode->_depthStencilAttachment->Range, depthRange));
    EXPECT_EQ(passNode->_depthStencilAttachment->DepthLoad, LoadAction::Load);
    EXPECT_EQ(passNode->_depthStencilAttachment->DepthStore, StoreAction::Discard);
    EXPECT_EQ(passNode->_depthStencilAttachment->StencilLoad, LoadAction::Load);
    EXPECT_EQ(passNode->_depthStencilAttachment->StencilStore, StoreAction::Discard);

    EXPECT_NE(
        FindTextureEdge(
            graph,
            pass,
            lightingTexture,
            RDGExecutionStage::ColorOutput,
            RDGMemoryAccess::ColorAttachmentWrite,
            RDGTextureLayout::ColorAttachment,
            colorRange),
        nullptr);
    EXPECT_NE(
        FindTextureEdge(
            graph,
            pass,
            depthTexture,
            RDGExecutionStage::DepthStencil,
            RDGMemoryAccess::DepthStencilRead,
            RDGTextureLayout::DepthStencilReadOnly,
            depthRange),
        nullptr);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            vertexBuffer,
            pass,
            RDGExecutionStage::VertexInput,
            RDGMemoryAccess::VertexRead,
            BufferRange{0, 1024}),
        nullptr);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            sceneCBuffer,
            pass,
            RDGExecutionStage::VertexShader,
            RDGMemoryAccess::ConstantRead,
            BufferRange{0, 256}),
        nullptr);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            sceneCBuffer,
            pass,
            RDGExecutionStage::PixelShader,
            RDGMemoryAccess::ConstantRead,
            BufferRange{0, 256}),
        nullptr);
    EXPECT_NE(
        FindTextureEdge(
            graph,
            blueNoiseTexture,
            pass,
            RDGExecutionStage::PixelShader,
            RDGMemoryAccess::ShaderRead,
            RDGTextureLayout::ShaderReadOnly,
            colorRange),
        nullptr);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            pass,
            storageBuffer,
            RDGExecutionStage::PixelShader,
            RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
            BufferRange{128, 256}),
        nullptr);
    EXPECT_NE(
        FindTextureEdge(
            graph,
            pass,
            historyTexture,
            RDGExecutionStage::PixelShader,
            RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
            RDGTextureLayout::General,
            colorRange),
        nullptr);

    const auto dot = graph.ExportGraphviz();
    ExpectContains(dot, "RasterPass");
    ExpectContains(dot, "queue=Direct");
    ExpectNotContains(dot, "colorAttachmentCount=");
    ExpectNotContains(dot, "hasDepthStencilAttachment=");
}

TEST(RenderGraphTest, ComputeAndCopyBuildersMapResourceUsageToExpectedEdges) {
    RenderGraph graph{};

    const auto inputBuffer = graph.AddBuffer(2048, "InputBuffer");
    const auto outputBuffer = graph.AddBuffer(4096, "OutputBuffer");
    const auto stagingBuffer = graph.AddBuffer(8192, "StagingBuffer");
    const auto inputTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        256,
        256,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        "InputTexture");
    const auto outputTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        256,
        256,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        "OutputTexture");

    RDGComputePassBuilder computeBuilder{&graph};
    const auto computeRange = SubresourceRange{0, 1, 0, 1};
    const auto computePass = computeBuilder
        .UseCBuffer(inputBuffer, BufferRange{0, 256})
        .UseBuffer(inputBuffer, BufferRange{256, 512})
        .UseRWBuffer(outputBuffer, BufferRange{0, 1024})
        .UseTexture(inputTexture, computeRange)
        .UseRWTexture(outputTexture, computeRange)
        .Build();

    EXPECT_EQ(computeBuilder.Build().Id, computePass.Id);

    auto* computePassNode = static_cast<RDGPassNode*>(graph._nodes[computePass.Id].get());
    ASSERT_NE(computePassNode, nullptr);
    EXPECT_EQ(computePassNode->_type, QueueType::Compute);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            inputBuffer,
            computePass,
            RDGExecutionStage::ComputeShader,
            RDGMemoryAccess::ConstantRead,
            BufferRange{0, 256}),
        nullptr);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            inputBuffer,
            computePass,
            RDGExecutionStage::ComputeShader,
            RDGMemoryAccess::ShaderRead,
            BufferRange{256, 512}),
        nullptr);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            computePass,
            outputBuffer,
            RDGExecutionStage::ComputeShader,
            RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
            BufferRange{0, 1024}),
        nullptr);
    EXPECT_NE(
        FindTextureEdge(
            graph,
            inputTexture,
            computePass,
            RDGExecutionStage::ComputeShader,
            RDGMemoryAccess::ShaderRead,
            RDGTextureLayout::ShaderReadOnly,
            computeRange),
        nullptr);
    EXPECT_NE(
        FindTextureEdge(
            graph,
            computePass,
            outputTexture,
            RDGExecutionStage::ComputeShader,
            RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
            RDGTextureLayout::General,
            computeRange),
        nullptr);

    RDGCopyPassBuilder copyBuilder{&graph};
    const auto copyRange = SubresourceRange{0, 1, 0, 1};
    const auto copyPass = copyBuilder
        .CopyBufferToBuffer(outputBuffer, 128, inputBuffer, 64, 512)
        .CopyBufferToTexture(outputTexture, copyRange, stagingBuffer, 256)
        .CopyTextureToBuffer(stagingBuffer, 1024, inputTexture, copyRange)
        .Build();

    EXPECT_EQ(copyBuilder.Build().Id, copyPass.Id);

    auto* copyPassNode = static_cast<RDGPassNode*>(graph._nodes[copyPass.Id].get());
    ASSERT_NE(copyPassNode, nullptr);
    EXPECT_EQ(copyPassNode->_type, QueueType::Copy);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            inputBuffer,
            copyPass,
            RDGExecutionStage::Copy,
            RDGMemoryAccess::TransferRead,
            BufferRange{64, 512}),
        nullptr);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            copyPass,
            outputBuffer,
            RDGExecutionStage::Copy,
            RDGMemoryAccess::TransferWrite,
            BufferRange{128, 512}),
        nullptr);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            stagingBuffer,
            copyPass,
            RDGExecutionStage::Copy,
            RDGMemoryAccess::TransferRead,
            BufferRange{256, BufferRange::All()}),
        nullptr);
    EXPECT_NE(
        FindTextureEdge(
            graph,
            copyPass,
            outputTexture,
            RDGExecutionStage::Copy,
            RDGMemoryAccess::TransferWrite,
            RDGTextureLayout::TransferDestination,
            copyRange),
        nullptr);
    EXPECT_NE(
        FindTextureEdge(
            graph,
            inputTexture,
            copyPass,
            RDGExecutionStage::Copy,
            RDGMemoryAccess::TransferRead,
            RDGTextureLayout::TransferSource,
            copyRange),
        nullptr);
    EXPECT_NE(
        FindBufferEdge(
            graph,
            copyPass,
            stagingBuffer,
            RDGExecutionStage::Copy,
            RDGMemoryAccess::TransferWrite,
            BufferRange{1024, BufferRange::All()}),
        nullptr);

    const auto validation = graph.Validate();
    ASSERT_TRUE(validation.first) << validation.second;
}

}  // namespace
