#include <string_view>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <radray/render/common.h>
#include <radray/runtime/render_graph.h>

using namespace radray;
using namespace radray::render;

namespace {

void ExpectContains(std::string_view text, std::string_view needle) {
    EXPECT_NE(text.find(needle), std::string_view::npos) << "missing substring: " << needle;
}

class ComplexRasterPass final : public IRDGRasterPass {
public:
    ComplexRasterPass(
        RDGBufferHandle vertexBuffer,
        RDGBufferHandle indexBuffer,
        RDGBufferHandle sceneBuffer,
        RDGTextureHandle historyTexture,
        RDGTextureHandle streamedAlbedo,
        RDGTextureHandle gbufferColor,
        RDGTextureHandle depthTexture) noexcept
        : _vertexBuffer(vertexBuffer),
          _indexBuffer(indexBuffer),
          _sceneBuffer(sceneBuffer),
          _historyTexture(historyTexture),
          _streamedAlbedo(streamedAlbedo),
          _gbufferColor(gbufferColor),
          _depthTexture(depthTexture) {}

    void Setup(Builder& builder) override {
        const auto allBuffer = BufferRange::AllRange();
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseVertexBuffer(_vertexBuffer, allBuffer)
            .UseIndexBuffer(_indexBuffer, allBuffer)
            .UseCBuffer(_sceneBuffer, ShaderStage::Graphics, allBuffer)
            .UseTexture(_historyTexture, ShaderStage::Pixel, allTexture)
            .UseTexture(_streamedAlbedo, ShaderStage::Pixel, allTexture)
            .UseColorAttachment(
                0,
                _gbufferColor,
                allTexture,
                LoadAction::Clear,
                StoreAction::Store,
                ColorClearValue{{0.05f, 0.1f, 0.15f, 1.0f}})
            .UseDepthStencilAttachment(
                _depthTexture,
                allTexture,
                LoadAction::Clear,
                StoreAction::Store,
                LoadAction::Clear,
                StoreAction::Store,
                DepthStencilClearValue{1.0f, uint8_t{0}});
    }

    void Execute(GraphicsCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _vertexBuffer{};
    RDGBufferHandle _indexBuffer{};
    RDGBufferHandle _sceneBuffer{};
    RDGTextureHandle _historyTexture{};
    RDGTextureHandle _streamedAlbedo{};
    RDGTextureHandle _gbufferColor{};
    RDGTextureHandle _depthTexture{};
};

class ComplexComputePass final : public IRDGComputePass {
public:
    ComplexComputePass(
        RDGBufferHandle sceneBuffer,
        RDGBufferHandle lightListBuffer,
        RDGTextureHandle gbufferColor,
        RDGTextureHandle depthTexture,
        RDGTextureHandle postProcessOutput) noexcept
        : _sceneBuffer(sceneBuffer),
          _lightListBuffer(lightListBuffer),
          _gbufferColor(gbufferColor),
          _depthTexture(depthTexture),
          _postProcessOutput(postProcessOutput) {}

    void Setup(Builder& builder) override {
        const auto allBuffer = BufferRange::AllRange();
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseCBuffer(_sceneBuffer, allBuffer)
            .UseTexture(_gbufferColor, allTexture)
            .UseTexture(_depthTexture, allTexture)
            .UseRWBuffer(_lightListBuffer, allBuffer)
            .UseRWTexture(_postProcessOutput, allTexture);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _sceneBuffer{};
    RDGBufferHandle _lightListBuffer{};
    RDGTextureHandle _gbufferColor{};
    RDGTextureHandle _depthTexture{};
    RDGTextureHandle _postProcessOutput{};
};

TEST(RenderGraphTest, ExportGraphvizIncludesNodeFieldsAndNormalEdges) {
    RenderGraph graph{};

    const auto input = graph.AddBuffer(64, MemoryType::Device, BufferUse::CBuffer, "input-buffer");
    const auto output = graph.AddBuffer(128, MemoryType::Device, BufferUse::Resource | BufferUse::UnorderedAccess, "output-buffer");
    const auto pass = graph.AddCopyPass("copy-pass");

    graph.Link(
        input,
        pass,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ConstantRead,
        BufferRange::AllRange());
    graph.Link(
        pass,
        output,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
        BufferRange::AllRange());

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "digraph RenderGraph {");
    ExpectContains(dot, "n0 [label=\"id: 0\\nname: input-buffer\\nkind: Resource\\nownership: Internal\\ntag: [Buffer]\"]");
    ExpectContains(dot, "n1 [label=\"id: 1\\nname: output-buffer\\nkind: Resource\\nownership: Internal\\ntag: [Buffer]\"]");
    ExpectContains(dot, "n2 [shape=ellipse, label=\"id: 2\\nname: copy-pass\\nkind: Pass\\ntag: [Copy]\"]");
    ExpectContains(dot, "n0 -> n2 [label=\"stage: [VertexShader | PixelShader]\\naccess: [ConstantRead]\"]");
    ExpectContains(dot, "n2 -> n1 [label=\"stage: [VertexShader | PixelShader]\\naccess: [ShaderRead | ShaderWrite]\"]");
}

TEST(RenderGraphTest, ExportGraphvizUsesEnumFlagsFormattingForCombinedEdgeState) {
    RenderGraph graph{};

    const auto input = graph.AddBuffer(64, MemoryType::Device, BufferUse::CBuffer, "buffer-a");
    const auto pass = graph.AddCopyPass("copy-pass");

    graph.Link(
        input,
        pass,
        RDGExecutionStage::VertexShader | RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
        BufferRange::AllRange());

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "stage: [VertexShader | PixelShader]");
    ExpectContains(dot, "access: [ShaderRead | ShaderWrite]");
}

TEST(RenderGraphTest, ExportGraphvizIncludesImportAndExportPseudoNodes) {
    RenderGraph graph{};

    const auto imported = graph.ImportBuffer(
        GpuBufferHandle{},
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        BufferRange::AllRange(),
        "external-buffer");
    graph.ExportBuffer(imported, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, BufferRange::AllRange());

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "n0 [label=\"id: 0\\nname: external-buffer\\nkind: Resource\\nownership: External\\ntag: [Buffer]\"]");
    ExpectContains(dot, "n0_import [shape=oval, style=dashed, label=\"kind: Import\\nownership: External\"]");
    ExpectContains(dot, "n0_export [shape=oval, style=dashed, label=\"kind: Export\\nownership: External\"]");
    ExpectContains(dot, "n0_import -> n0 [label=\"stage: [Host]\\naccess: [HostWrite]\"]");
    ExpectContains(dot, "n0 -> n0_export [label=\"stage: [Host]\\naccess: [HostRead]\"]");
}

TEST(RenderGraphTest, ExportGraphvizEscapesNodeNames) {
    RenderGraph graph{};

    graph.AddBuffer(16, MemoryType::Device, BufferUse::Common, "buffer \"quoted\"\nnext");

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "name: buffer \\\"quoted\\\"\\nnext");
}

TEST(RenderGraphTest, ExportGraphvizHandlesComplexMixedPassGraph) {
    RenderGraph graph{};

    const auto allBuffer = BufferRange::AllRange();
    const auto allTexture = SubresourceRange::AllSub();

    const auto uploadBuffer = graph.ImportBuffer(
        GpuBufferHandle{},
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        allBuffer,
        "upload-buffer");
    const auto historyTexture = graph.ImportTexture(
        GpuTextureHandle{},
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        RDGTextureLayout::ShaderReadOnly,
        allTexture,
        "history-texture");

    const auto sceneBuffer = graph.AddBuffer(256, MemoryType::Device, BufferUse::CBuffer, "scene-constants");
    const auto vertexBuffer = graph.AddBuffer(4096, MemoryType::Device, BufferUse::Vertex | BufferUse::CopyDestination, "vertex-buffer");
    const auto indexBuffer = graph.AddBuffer(2048, MemoryType::Device, BufferUse::Index | BufferUse::CopyDestination, "index-buffer");
    const auto lightListBuffer = graph.AddBuffer(
        8192,
        MemoryType::Device,
        BufferUse::UnorderedAccess | BufferUse::CopySource,
        "light-list-buffer");
    const auto readbackBuffer = graph.AddBuffer(16384, MemoryType::ReadBack, BufferUse::CopyDestination, "readback-buffer");

    const auto streamedAlbedo = graph.AddTexture(
        TextureDimension::Dim2D,
        1024,
        1024,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::CopyDestination | TextureUse::Resource,
        "streamed-albedo");
    const auto gbufferColor = graph.AddTexture(
        TextureDimension::Dim2D,
        1280,
        720,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::Resource,
        "gbuffer-color");
    const auto depthTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        1280,
        720,
        1,
        1,
        1,
        TextureFormat::D32_FLOAT,
        MemoryType::Device,
        TextureUse::DepthStencilWrite | TextureUse::Resource,
        "scene-depth");
    const auto postProcessOutput = graph.AddTexture(
        TextureDimension::Dim2D,
        1280,
        720,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::UnorderedAccess | TextureUse::CopySource | TextureUse::Resource,
        "post-process-output");

    const auto uploadCopyPass = graph.AddCopyPass("upload-copy");
    graph.Link(uploadBuffer, uploadCopyPass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, allBuffer);
    graph.Link(uploadCopyPass, vertexBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.Link(uploadCopyPass, indexBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.Link(
        uploadCopyPass,
        streamedAlbedo,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        RDGTextureLayout::TransferDestination,
        allTexture);

    graph.AddRasterPass(
        "gbuffer-pass",
        make_unique<ComplexRasterPass>(
            vertexBuffer,
            indexBuffer,
            sceneBuffer,
            historyTexture,
            streamedAlbedo,
            gbufferColor,
            depthTexture));

    graph.AddComputePass(
        "post-compute",
        make_unique<ComplexComputePass>(
            sceneBuffer,
            lightListBuffer,
            gbufferColor,
            depthTexture,
            postProcessOutput));

    const auto readbackCopyPass = graph.AddCopyPass("readback-copy");
    graph.Link(lightListBuffer, readbackCopyPass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, allBuffer);
    graph.Link(
        postProcessOutput,
        readbackCopyPass,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        allTexture);
    graph.Link(readbackCopyPass, readbackBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    graph.ExportBuffer(readbackBuffer, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, allBuffer);
    graph.ExportTexture(
        postProcessOutput,
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostRead,
        RDGTextureLayout::General,
        allTexture);

    const auto dot = graph.ExportGraphviz();

    ExpectContains(dot, "name: upload-copy\\nkind: Pass\\ntag: [Copy]");
    ExpectContains(dot, "name: gbuffer-pass\\nkind: Pass\\ntag: [Direct]");
    ExpectContains(dot, "name: post-compute\\nkind: Pass\\ntag: [Compute]");
    ExpectContains(dot, "name: readback-copy\\nkind: Pass\\ntag: [Copy]");
    ExpectContains(dot, "shape=ellipse, label=\"id: ");

    ExpectContains(dot, "name: upload-buffer\\nkind: Resource\\nownership: External\\ntag: [Buffer]");
    ExpectContains(dot, "name: history-texture\\nkind: Resource\\nownership: External\\ntag: [Texture]");
    ExpectContains(dot, "name: streamed-albedo\\nkind: Resource\\nownership: Internal\\ntag: [Texture]");
    ExpectContains(dot, "name: gbuffer-color\\nkind: Resource\\nownership: Internal\\ntag: [Texture]");
    ExpectContains(dot, "name: light-list-buffer\\nkind: Resource\\nownership: Internal\\ntag: [Buffer]");
    ExpectContains(dot, "name: post-process-output\\nkind: Resource\\nownership: Internal\\ntag: [Texture]");
    ExpectContains(dot, "name: readback-buffer\\nkind: Resource\\nownership: Internal\\ntag: [Buffer]");

    ExpectContains(dot, "kind: Import\\nownership: External");
    ExpectContains(dot, "kind: Export\\nownership: Internal");
    ExpectContains(dot, "stage: [Host]\\naccess: [HostWrite]");
    ExpectContains(dot, "stage: [Host]\\naccess: [HostRead]");

    ExpectContains(dot, "stage: [Copy]\\naccess: [TransferRead]");
    ExpectContains(dot, "stage: [Copy]\\naccess: [TransferWrite]");
    ExpectContains(dot, "stage: [VertexInput]\\naccess: [VertexRead]");
    ExpectContains(dot, "stage: [VertexInput]\\naccess: [IndexRead]");
    ExpectContains(dot, "stage: [VertexShader | PixelShader]\\naccess: [ConstantRead]");
    ExpectContains(dot, "stage: [PixelShader]\\naccess: [ShaderRead]");
    ExpectContains(dot, "stage: [ColorOutput]\\naccess: [ColorAttachmentWrite]");
    ExpectContains(dot, "stage: [DepthStencil]\\naccess: [DepthStencilWrite]");
    ExpectContains(dot, "stage: [ComputeShader]\\naccess: [ShaderRead | ShaderWrite]");

    auto c = graph.Compile();
}

}  // namespace
