#include <string>
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

std::string ExtractNodeAttribute(std::string_view dot, std::string_view nodeId, std::string_view attribute) {
    const auto nodePrefix = fmt::format("    {} [", nodeId);
    const auto nodePos = dot.find(nodePrefix);
    if (nodePos == std::string_view::npos) {
        return {};
    }

    const auto lineEnd = dot.find("];", nodePos);
    if (lineEnd == std::string_view::npos) {
        return {};
    }

    const auto attrPrefix = fmt::format("{}=\"", attribute);
    const auto line = dot.substr(nodePos, lineEnd - nodePos);
    const auto attrPos = line.find(attrPrefix);
    if (attrPos == std::string_view::npos) {
        return {};
    }

    const auto valueBegin = attrPos + attrPrefix.size();
    const auto valueEnd = line.find('"', valueBegin);
    if (valueEnd == std::string_view::npos) {
        return {};
    }

    return std::string(line.substr(valueBegin, valueEnd - valueBegin));
}

GpuBufferHandle MakeGpuBufferHandle(Buffer* buffer) {
    GpuBufferHandle handle{};
    handle.Handle = 0;
    handle.NativeHandle = buffer;
    return handle;
}

GpuTextureHandle MakeGpuTextureHandle(Texture* texture) {
    GpuTextureHandle handle{};
    handle.Handle = 0;
    handle.NativeHandle = texture;
    return handle;
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

class RWBufferComputePass final : public IRDGComputePass {
public:
    explicit RWBufferComputePass(RDGBufferHandle buffer) noexcept
        : _buffer(buffer) {}

    void Setup(Builder& builder) override {
        builder.UseRWBuffer(_buffer, BufferRange::AllRange());
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _buffer{};
};

class FakeBuffer final : public Buffer {
public:
    explicit FakeBuffer(BufferDescriptor desc) noexcept
        : _desc(desc) {}

    bool IsValid() const noexcept override {
        return !_destroyed;
    }

    void Destroy() noexcept override {
        _destroyed = true;
    }

    void SetDebugName(std::string_view name) noexcept override {
        _debugName = std::string(name);
    }

    void* Map(uint64_t offset, uint64_t size) noexcept override {
        (void)offset;
        (void)size;
        return nullptr;
    }

    void Unmap(uint64_t offset, uint64_t size) noexcept override {
        (void)offset;
        (void)size;
    }

    BufferDescriptor GetDesc() const noexcept override {
        return _desc;
    }

private:
    BufferDescriptor _desc{};
    std::string _debugName{};
    bool _destroyed{false};
};

class FakeTexture final : public Texture {
public:
    explicit FakeTexture(TextureDescriptor desc) noexcept
        : _desc(desc) {}

    bool IsValid() const noexcept override {
        return !_destroyed;
    }

    void Destroy() noexcept override {
        _destroyed = true;
    }

    void SetDebugName(std::string_view name) noexcept override {
        _debugName = std::string(name);
    }

    TextureDescriptor GetDesc() const noexcept override {
        return _desc;
    }

private:
    TextureDescriptor _desc{};
    std::string _debugName{};
    bool _destroyed{false};
};

class ShadowRasterPass final : public IRDGRasterPass {
public:
    ShadowRasterPass(
        RDGBufferHandle sceneBuffer,
        RDGBufferHandle drawArgsBuffer,
        RDGBufferHandle vertexBuffer,
        RDGBufferHandle indexBuffer,
        RDGBufferHandle instanceBuffer,
        RDGTextureHandle albedoAtlas,
        RDGTextureHandle shadowAtlas) noexcept
        : _sceneBuffer(sceneBuffer),
          _drawArgsBuffer(drawArgsBuffer),
          _vertexBuffer(vertexBuffer),
          _indexBuffer(indexBuffer),
          _instanceBuffer(instanceBuffer),
          _albedoAtlas(albedoAtlas),
          _shadowAtlas(shadowAtlas) {}

    void Setup(Builder& builder) override {
        const auto allBuffer = BufferRange::AllRange();
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseCBuffer(_sceneBuffer, ShaderStage::Graphics, allBuffer)
            .UseIndirectBuffer(_drawArgsBuffer, allBuffer)
            .UseVertexBuffer(_vertexBuffer, allBuffer)
            .UseIndexBuffer(_indexBuffer, allBuffer)
            .UseBuffer(_instanceBuffer, ShaderStage::Vertex, allBuffer)
            .UseTexture(_albedoAtlas, ShaderStage::Pixel, allTexture)
            .UseDepthStencilAttachment(
                _shadowAtlas,
                allTexture,
                LoadAction::Clear,
                StoreAction::Store,
                LoadAction::DontCare,
                StoreAction::Discard,
                DepthStencilClearValue{1.0f, uint8_t{0}});
    }

    void Execute(GraphicsCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _sceneBuffer{};
    RDGBufferHandle _drawArgsBuffer{};
    RDGBufferHandle _vertexBuffer{};
    RDGBufferHandle _indexBuffer{};
    RDGBufferHandle _instanceBuffer{};
    RDGTextureHandle _albedoAtlas{};
    RDGTextureHandle _shadowAtlas{};
};

class GBufferRasterPass final : public IRDGRasterPass {
public:
    GBufferRasterPass(
        RDGBufferHandle sceneBuffer,
        RDGBufferHandle drawArgsBuffer,
        RDGBufferHandle vertexBuffer,
        RDGBufferHandle indexBuffer,
        RDGBufferHandle instanceBuffer,
        RDGBufferHandle materialBuffer,
        RDGTextureHandle albedoAtlas,
        RDGTextureHandle gbufferAlbedo,
        RDGTextureHandle gbufferNormal,
        RDGTextureHandle motionVectors,
        RDGTextureHandle sceneDepth) noexcept
        : _sceneBuffer(sceneBuffer),
          _drawArgsBuffer(drawArgsBuffer),
          _vertexBuffer(vertexBuffer),
          _indexBuffer(indexBuffer),
          _instanceBuffer(instanceBuffer),
          _materialBuffer(materialBuffer),
          _albedoAtlas(albedoAtlas),
          _gbufferAlbedo(gbufferAlbedo),
          _gbufferNormal(gbufferNormal),
          _motionVectors(motionVectors),
          _sceneDepth(sceneDepth) {}

    void Setup(Builder& builder) override {
        const auto allBuffer = BufferRange::AllRange();
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseCBuffer(_sceneBuffer, ShaderStage::Graphics, allBuffer)
            .UseIndirectBuffer(_drawArgsBuffer, allBuffer)
            .UseVertexBuffer(_vertexBuffer, allBuffer)
            .UseIndexBuffer(_indexBuffer, allBuffer)
            .UseBuffer(_instanceBuffer, ShaderStage::Vertex, allBuffer)
            .UseBuffer(_materialBuffer, ShaderStage::Pixel, allBuffer)
            .UseTexture(_albedoAtlas, ShaderStage::Pixel, allTexture)
            .UseColorAttachment(
                0,
                _gbufferAlbedo,
                allTexture,
                LoadAction::Clear,
                StoreAction::Store,
                ColorClearValue{{0.0f, 0.0f, 0.0f, 1.0f}})
            .UseColorAttachment(
                1,
                _gbufferNormal,
                allTexture,
                LoadAction::Clear,
                StoreAction::Store,
                ColorClearValue{{0.5f, 0.5f, 1.0f, 1.0f}})
            .UseColorAttachment(
                2,
                _motionVectors,
                allTexture,
                LoadAction::Clear,
                StoreAction::Store,
                ColorClearValue{{0.0f, 0.0f, 0.0f, 0.0f}})
            .UseDepthStencilAttachment(
                _sceneDepth,
                allTexture,
                LoadAction::Clear,
                StoreAction::Store,
                LoadAction::DontCare,
                StoreAction::Discard,
                DepthStencilClearValue{1.0f, uint8_t{0}});
    }

    void Execute(GraphicsCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _sceneBuffer{};
    RDGBufferHandle _drawArgsBuffer{};
    RDGBufferHandle _vertexBuffer{};
    RDGBufferHandle _indexBuffer{};
    RDGBufferHandle _instanceBuffer{};
    RDGBufferHandle _materialBuffer{};
    RDGTextureHandle _albedoAtlas{};
    RDGTextureHandle _gbufferAlbedo{};
    RDGTextureHandle _gbufferNormal{};
    RDGTextureHandle _motionVectors{};
    RDGTextureHandle _sceneDepth{};
};

class HiZBuildComputePass final : public IRDGComputePass {
public:
    HiZBuildComputePass(RDGTextureHandle sceneDepth, RDGTextureHandle depthPyramid) noexcept
        : _sceneDepth(sceneDepth),
          _depthPyramid(depthPyramid) {}

    void Setup(Builder& builder) override {
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseTexture(_sceneDepth, allTexture)
            .UseRWTexture(_depthPyramid, allTexture);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGTextureHandle _sceneDepth{};
    RDGTextureHandle _depthPyramid{};
};

class SsaoComputePass final : public IRDGComputePass {
public:
    SsaoComputePass(
        RDGTextureHandle depthPyramid,
        RDGTextureHandle gbufferNormal,
        RDGTextureHandle blueNoise,
        RDGTextureHandle ssao) noexcept
        : _depthPyramid(depthPyramid),
          _gbufferNormal(gbufferNormal),
          _blueNoise(blueNoise),
          _ssao(ssao) {}

    void Setup(Builder& builder) override {
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseTexture(_depthPyramid, allTexture)
            .UseTexture(_gbufferNormal, allTexture)
            .UseTexture(_blueNoise, allTexture)
            .UseRWTexture(_ssao, allTexture);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGTextureHandle _depthPyramid{};
    RDGTextureHandle _gbufferNormal{};
    RDGTextureHandle _blueNoise{};
    RDGTextureHandle _ssao{};
};

class LightCullingComputePass final : public IRDGComputePass {
public:
    LightCullingComputePass(
        RDGBufferHandle sceneBuffer,
        RDGBufferHandle lightBuffer,
        RDGTextureHandle depthPyramid,
        RDGBufferHandle lightGrid,
        RDGBufferHandle visibleLights) noexcept
        : _sceneBuffer(sceneBuffer),
          _lightBuffer(lightBuffer),
          _depthPyramid(depthPyramid),
          _lightGrid(lightGrid),
          _visibleLights(visibleLights) {}

    void Setup(Builder& builder) override {
        const auto allBuffer = BufferRange::AllRange();
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseCBuffer(_sceneBuffer, allBuffer)
            .UseBuffer(_lightBuffer, allBuffer)
            .UseTexture(_depthPyramid, allTexture)
            .UseRWBuffer(_lightGrid, allBuffer)
            .UseRWBuffer(_visibleLights, allBuffer);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _sceneBuffer{};
    RDGBufferHandle _lightBuffer{};
    RDGTextureHandle _depthPyramid{};
    RDGBufferHandle _lightGrid{};
    RDGBufferHandle _visibleLights{};
};

class DeferredLightingComputePass final : public IRDGComputePass {
public:
    DeferredLightingComputePass(
        RDGBufferHandle sceneBuffer,
        RDGBufferHandle lightGrid,
        RDGBufferHandle visibleLights,
        RDGTextureHandle gbufferAlbedo,
        RDGTextureHandle gbufferNormal,
        RDGTextureHandle sceneDepth,
        RDGTextureHandle shadowAtlas,
        RDGTextureHandle ssao,
        RDGTextureHandle environmentMap,
        RDGTextureHandle hdrScene,
        RDGBufferHandle luminanceHistogram) noexcept
        : _sceneBuffer(sceneBuffer),
          _lightGrid(lightGrid),
          _visibleLights(visibleLights),
          _gbufferAlbedo(gbufferAlbedo),
          _gbufferNormal(gbufferNormal),
          _sceneDepth(sceneDepth),
          _shadowAtlas(shadowAtlas),
          _ssao(ssao),
          _environmentMap(environmentMap),
          _hdrScene(hdrScene),
          _luminanceHistogram(luminanceHistogram) {}

    void Setup(Builder& builder) override {
        const auto allBuffer = BufferRange::AllRange();
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseCBuffer(_sceneBuffer, allBuffer)
            .UseBuffer(_lightGrid, allBuffer)
            .UseBuffer(_visibleLights, allBuffer)
            .UseTexture(_gbufferAlbedo, allTexture)
            .UseTexture(_gbufferNormal, allTexture)
            .UseTexture(_sceneDepth, allTexture)
            .UseTexture(_shadowAtlas, allTexture)
            .UseTexture(_ssao, allTexture)
            .UseTexture(_environmentMap, allTexture)
            .UseRWTexture(_hdrScene, allTexture)
            .UseRWBuffer(_luminanceHistogram, allBuffer);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _sceneBuffer{};
    RDGBufferHandle _lightGrid{};
    RDGBufferHandle _visibleLights{};
    RDGTextureHandle _gbufferAlbedo{};
    RDGTextureHandle _gbufferNormal{};
    RDGTextureHandle _sceneDepth{};
    RDGTextureHandle _shadowAtlas{};
    RDGTextureHandle _ssao{};
    RDGTextureHandle _environmentMap{};
    RDGTextureHandle _hdrScene{};
    RDGBufferHandle _luminanceHistogram{};
};

class ExposureComputePass final : public IRDGComputePass {
public:
    ExposureComputePass(RDGBufferHandle luminanceHistogram, RDGBufferHandle exposureState) noexcept
        : _luminanceHistogram(luminanceHistogram),
          _exposureState(exposureState) {}

    void Setup(Builder& builder) override {
        const auto allBuffer = BufferRange::AllRange();
        builder
            .UseBuffer(_luminanceHistogram, allBuffer)
            .UseRWBuffer(_exposureState, allBuffer);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _luminanceHistogram{};
    RDGBufferHandle _exposureState{};
};

class TaaResolveComputePass final : public IRDGComputePass {
public:
    TaaResolveComputePass(
        RDGBufferHandle exposureState,
        RDGTextureHandle hdrScene,
        RDGTextureHandle motionVectors,
        RDGTextureHandle historyColor,
        RDGTextureHandle taaHdr,
        RDGTextureHandle historyNext) noexcept
        : _exposureState(exposureState),
          _hdrScene(hdrScene),
          _motionVectors(motionVectors),
          _historyColor(historyColor),
          _taaHdr(taaHdr),
          _historyNext(historyNext) {}

    void Setup(Builder& builder) override {
        const auto allBuffer = BufferRange::AllRange();
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseBuffer(_exposureState, allBuffer)
            .UseTexture(_hdrScene, allTexture)
            .UseTexture(_motionVectors, allTexture)
            .UseTexture(_historyColor, allTexture)
            .UseRWTexture(_taaHdr, allTexture)
            .UseRWTexture(_historyNext, allTexture);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _exposureState{};
    RDGTextureHandle _hdrScene{};
    RDGTextureHandle _motionVectors{};
    RDGTextureHandle _historyColor{};
    RDGTextureHandle _taaHdr{};
    RDGTextureHandle _historyNext{};
};

class BloomPrefilterComputePass final : public IRDGComputePass {
public:
    BloomPrefilterComputePass(RDGTextureHandle taaHdr, RDGTextureHandle bloomPrefilter) noexcept
        : _taaHdr(taaHdr),
          _bloomPrefilter(bloomPrefilter) {}

    void Setup(Builder& builder) override {
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseTexture(_taaHdr, allTexture)
            .UseRWTexture(_bloomPrefilter, allTexture);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGTextureHandle _taaHdr{};
    RDGTextureHandle _bloomPrefilter{};
};

class BloomBlurComputePass final : public IRDGComputePass {
public:
    BloomBlurComputePass(RDGTextureHandle bloomPrefilter, RDGTextureHandle bloomBlurred) noexcept
        : _bloomPrefilter(bloomPrefilter),
          _bloomBlurred(bloomBlurred) {}

    void Setup(Builder& builder) override {
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseTexture(_bloomPrefilter, allTexture)
            .UseRWTexture(_bloomBlurred, allTexture);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGTextureHandle _bloomPrefilter{};
    RDGTextureHandle _bloomBlurred{};
};

class ToneMapComputePass final : public IRDGComputePass {
public:
    ToneMapComputePass(
        RDGBufferHandle exposureState,
        RDGTextureHandle taaHdr,
        RDGTextureHandle bloomBlurred,
        RDGTextureHandle uiOverlay,
        RDGTextureHandle finalLdr) noexcept
        : _exposureState(exposureState),
          _taaHdr(taaHdr),
          _bloomBlurred(bloomBlurred),
          _uiOverlay(uiOverlay),
          _finalLdr(finalLdr) {}

    void Setup(Builder& builder) override {
        const auto allBuffer = BufferRange::AllRange();
        const auto allTexture = SubresourceRange::AllSub();
        builder
            .UseBuffer(_exposureState, allBuffer)
            .UseTexture(_taaHdr, allTexture)
            .UseTexture(_bloomBlurred, allTexture)
            .UseTexture(_uiOverlay, allTexture)
            .UseRWTexture(_finalLdr, allTexture);
    }

    void Execute(ComputeCommandEncoder* encoder, GpuAsyncContext* context) override {
        (void)encoder;
        (void)context;
    }

private:
    RDGBufferHandle _exposureState{};
    RDGTextureHandle _taaHdr{};
    RDGTextureHandle _bloomBlurred{};
    RDGTextureHandle _uiOverlay{};
    RDGTextureHandle _finalLdr{};
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
    FakeBuffer externalBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CopySource,
    });

    const auto imported = graph.ImportBuffer(
        MakeGpuBufferHandle(&externalBuffer),
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
    FakeBuffer uploadStaging(BufferDescriptor{
        .Size = 4096,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CopySource,
    });
    FakeTexture historyTextureBacking(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = 1280,
        .Height = 720,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA16_FLOAT,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
    });

    const auto uploadBuffer = graph.ImportBuffer(
        MakeGpuBufferHandle(&uploadStaging),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        allBuffer,
        "upload-buffer");
    const auto historyTexture = graph.ImportTexture(
        MakeGpuTextureHandle(&historyTextureBacking),
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
}

TEST(RenderGraphTest, CompileSkipsIsolatedPassesAndResources) {
    RenderGraph graph{};
    const auto allBuffer = BufferRange::AllRange();

    const auto input = graph.AddBuffer(64, MemoryType::Device, BufferUse::CopySource, "input");
    const auto output = graph.AddBuffer(64, MemoryType::Device, BufferUse::CopyDestination, "output");
    graph.AddBuffer(16, MemoryType::Device, BufferUse::Common, "unused");

    const auto copyPass = graph.AddCopyPass("copy");
    graph.Link(input, copyPass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, allBuffer);
    graph.Link(copyPass, output, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    graph.AddCopyPass("isolated-pass");
}

TEST(RenderGraphTest, ExportCompiledGraphvizIncludesVersionedResourcesAndExecutionOrder) {
    RenderGraph graph{};
    const auto allBuffer = BufferRange::AllRange();
    FakeBuffer inputBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CopySource,
    });

    const auto input = graph.ImportBuffer(
        MakeGpuBufferHandle(&inputBuffer),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        allBuffer,
        "input");
    const auto output = graph.AddBuffer(
        64,
        MemoryType::Device,
        BufferUse::CopyDestination,
        "output");
    const auto copyPass = graph.AddCopyPass("copy-pass");

    graph.Link(input, copyPass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, allBuffer);
    graph.Link(copyPass, output, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.ExportBuffer(output, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, allBuffer);

    const auto dot = graph.Compile().ExportCompiledGraphviz();

    ExpectContains(dot, "digraph CompiledRenderGraph {");
    ExpectContains(dot, "label=\"name: input#0\\nownership: External\\ntag: [Buffer]\"");
    ExpectContains(dot, "label=\"name: output#1\\nownership: Internal\\ntag: [Buffer]\"");
    ExpectContains(dot, "shape=ellipse, style=filled, fillcolor=\"#E8E8E8\", label=\"exec: 0\\nname: copy-pass\\nkind: Pass\\ntag: [Copy]\"");
    ExpectContains(dot, "r0v0 -> p0 [label=\"stage: [Copy]\\naccess: [TransferRead]\\nrange: All\"]");
    ExpectContains(dot, "p0 -> r1v1 [label=\"stage: [Copy]\\naccess: [TransferWrite]\\nrange: All\"]");
}

TEST(RenderGraphTest, ExportCompiledGraphvizReusesSameColorAcrossResourceVersions) {
    RenderGraph graph{};
    const auto allBuffer = BufferRange::AllRange();
    FakeBuffer uploadBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CopySource,
    });

    const auto upload = graph.ImportBuffer(
        MakeGpuBufferHandle(&uploadBuffer),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        allBuffer,
        "upload");
    const auto history = graph.AddBuffer(
        64,
        MemoryType::Device,
        BufferUse::CopyDestination | BufferUse::Resource | BufferUse::UnorderedAccess,
        "history");

    const auto initPass = graph.AddCopyPass("init");
    graph.Link(upload, initPass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, allBuffer);
    graph.Link(initPass, history, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.AddComputePass("history-rw", make_unique<RWBufferComputePass>(history));
    graph.ExportBuffer(history, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, allBuffer);

    const auto dot = graph.Compile().ExportCompiledGraphviz();

    ExpectContains(dot, "label=\"name: history#0\\nownership: Internal\\ntag: [Buffer]\"");
    ExpectContains(dot, "label=\"name: history#1\\nownership: Internal\\ntag: [Buffer]\"");
    ExpectContains(dot, "label=\"name: history#2\\nownership: Internal\\ntag: [Buffer]\"");
    ExpectContains(dot, "p0 -> p1 [style=dashed, color=\"#9A9A9A\"]");

    const auto colorV1 = ExtractNodeAttribute(dot, "r1v1", "fillcolor");
    const auto colorV2 = ExtractNodeAttribute(dot, "r1v2", "fillcolor");
    EXPECT_FALSE(colorV1.empty());
    EXPECT_EQ(colorV1, colorV2);
}

TEST(RenderGraphTest, ExportCompiledGraphvizHandlesReadWriteImportExportAndEscapedNames) {
    RenderGraph graph{};
    const auto allBuffer = BufferRange::AllRange();
    FakeBuffer rwBuffer(BufferDescriptor{
        .Size = 64,
        .Memory = MemoryType::Device,
        .Usage = BufferUse::Resource | BufferUse::UnorderedAccess,
    });

    const auto buffer = graph.ImportBuffer(
        MakeGpuBufferHandle(&rwBuffer),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        allBuffer,
        "buffer \"quoted\"\nnext");
    graph.AddComputePass("rw-pass", make_unique<RWBufferComputePass>(buffer));
    graph.ExportBuffer(buffer, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, allBuffer);

    const auto dot = graph.Compile().ExportCompiledGraphviz();

    ExpectContains(dot, "name: buffer \\\"quoted\\\"\\nnext#0");
    ExpectContains(dot, "r0_import -> r0v0 [label=\"stage: [Host]\\naccess: [HostWrite]\\nrange: All\"]");
    ExpectContains(dot, "r0v0 -> p0 [label=\"stage: [ComputeShader]\\naccess: [ShaderRead | ShaderWrite]\\nrange: All\"]");
    ExpectContains(dot, "p0 -> r0v1 [label=\"stage: [ComputeShader]\\naccess: [ShaderRead | ShaderWrite]\\nrange: All\"]");
    ExpectContains(dot, "r0v1 -> r0_export [label=\"stage: [Host]\\naccess: [HostRead]\\nrange: All\"]");
}

TEST(RenderGraphTest, ExportCompiledGraphvizHandlesLargeModernFrameGraph) {
    RenderGraph graph{};
    const auto allBuffer = BufferRange::AllRange();
    const auto allTexture = SubresourceRange::AllSub();

    FakeBuffer uploadStaging(BufferDescriptor{
        .Size = 1 << 20,
        .Memory = MemoryType::Upload,
        .Usage = BufferUse::CopySource,
    });
    FakeBuffer exposureStateBacking(BufferDescriptor{
        .Size = 256,
        .Memory = MemoryType::Device,
        .Usage = BufferUse::Resource | BufferUse::UnorderedAccess,
    });
    FakeTexture historyColorBacking(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = 1920,
        .Height = 1080,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA16_FLOAT,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
    });
    FakeTexture blueNoiseBacking(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = 128,
        .Height = 128,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
    });
    FakeTexture environmentMapBacking(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = 1024,
        .Height = 1024,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA16_FLOAT,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
    });
    FakeTexture uiOverlayBacking(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = 1920,
        .Height = 1080,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
    });

    const auto uploadBuffer = graph.ImportBuffer(
        MakeGpuBufferHandle(&uploadStaging),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        allBuffer,
        "upload-staging");
    const auto exposureState = graph.ImportBuffer(
        MakeGpuBufferHandle(&exposureStateBacking),
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostWrite,
        allBuffer,
        "exposure-state");
    const auto historyColor = graph.ImportTexture(
        MakeGpuTextureHandle(&historyColorBacking),
        RDGExecutionStage::ComputeShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        allTexture,
        "history-color");
    const auto blueNoise = graph.ImportTexture(
        MakeGpuTextureHandle(&blueNoiseBacking),
        RDGExecutionStage::ComputeShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        allTexture,
        "blue-noise");
    const auto environmentMap = graph.ImportTexture(
        MakeGpuTextureHandle(&environmentMapBacking),
        RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        allTexture,
        "environment-map");
    const auto uiOverlay = graph.ImportTexture(
        MakeGpuTextureHandle(&uiOverlayBacking),
        RDGExecutionStage::PixelShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        allTexture,
        "ui-overlay");

    const auto sceneBuffer = graph.AddBuffer(
        4096,
        MemoryType::Device,
        BufferUse::CBuffer | BufferUse::CopyDestination,
        "scene-buffer");
    const auto vertexBuffer = graph.AddBuffer(
        1 << 18,
        MemoryType::Device,
        BufferUse::Vertex | BufferUse::CopyDestination,
        "vertex-buffer");
    const auto indexBuffer = graph.AddBuffer(
        1 << 17,
        MemoryType::Device,
        BufferUse::Index | BufferUse::CopyDestination,
        "index-buffer");
    const auto instanceBuffer = graph.AddBuffer(
        1 << 16,
        MemoryType::Device,
        BufferUse::Resource | BufferUse::CopyDestination,
        "instance-buffer");
    const auto materialBuffer = graph.AddBuffer(
        1 << 16,
        MemoryType::Device,
        BufferUse::Resource | BufferUse::CopyDestination,
        "material-buffer");
    const auto drawArgsBuffer = graph.AddBuffer(
        4096,
        MemoryType::Device,
        BufferUse::Indirect | BufferUse::Resource | BufferUse::CopyDestination,
        "draw-args-buffer");
    const auto lightBuffer = graph.AddBuffer(
        1 << 15,
        MemoryType::Device,
        BufferUse::Resource | BufferUse::CopyDestination,
        "light-buffer");
    const auto lightGrid = graph.AddBuffer(
        1 << 15,
        MemoryType::Device,
        BufferUse::Resource | BufferUse::UnorderedAccess,
        "light-grid");
    const auto visibleLights = graph.AddBuffer(
        1 << 16,
        MemoryType::Device,
        BufferUse::Resource | BufferUse::UnorderedAccess,
        "visible-lights");
    const auto luminanceHistogram = graph.AddBuffer(
        4096,
        MemoryType::Device,
        BufferUse::Resource | BufferUse::UnorderedAccess,
        "luminance-histogram");
    const auto readbackBuffer = graph.AddBuffer(
        1 << 20,
        MemoryType::ReadBack,
        BufferUse::CopyDestination,
        "final-readback");

    const auto albedoAtlas = graph.AddTexture(
        TextureDimension::Dim2D,
        2048,
        2048,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::CopyDestination | TextureUse::Resource,
        "albedo-atlas");
    const auto shadowAtlas = graph.AddTexture(
        TextureDimension::Dim2D,
        4096,
        4096,
        1,
        1,
        1,
        TextureFormat::D32_FLOAT,
        MemoryType::Device,
        TextureUse::DepthStencilWrite | TextureUse::Resource,
        "shadow-atlas");
    const auto gbufferAlbedo = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::Resource,
        "gbuffer-albedo");
    const auto gbufferNormal = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::Resource,
        "gbuffer-normal");
    const auto motionVectors = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::RenderTarget | TextureUse::Resource,
        "motion-vectors");
    const auto sceneDepth = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::D32_FLOAT,
        MemoryType::Device,
        TextureUse::DepthStencilWrite | TextureUse::Resource,
        "scene-depth");
    const auto depthPyramid = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::UnorderedAccess | TextureUse::Resource,
        "depth-pyramid");
    const auto ssaoTexture = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::UnorderedAccess | TextureUse::Resource,
        "ssao");
    const auto hdrScene = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::UnorderedAccess | TextureUse::Resource,
        "hdr-scene");
    const auto taaHdr = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::UnorderedAccess | TextureUse::Resource,
        "taa-hdr");
    const auto historyNext = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::UnorderedAccess | TextureUse::Resource,
        "history-next");
    const auto bloomPrefilter = graph.AddTexture(
        TextureDimension::Dim2D,
        960,
        540,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::UnorderedAccess | TextureUse::Resource,
        "bloom-prefilter");
    const auto bloomBlurred = graph.AddTexture(
        TextureDimension::Dim2D,
        960,
        540,
        1,
        1,
        1,
        TextureFormat::RGBA16_FLOAT,
        MemoryType::Device,
        TextureUse::UnorderedAccess | TextureUse::Resource,
        "bloom-blurred");
    const auto finalLdr = graph.AddTexture(
        TextureDimension::Dim2D,
        1920,
        1080,
        1,
        1,
        1,
        TextureFormat::RGBA8_UNORM,
        MemoryType::Device,
        TextureUse::UnorderedAccess | TextureUse::CopySource | TextureUse::Resource,
        "final-ldr");

    const auto uploadPass = graph.AddCopyPass("upload-resources");
    graph.Link(uploadBuffer, uploadPass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, allBuffer);
    graph.Link(uploadPass, sceneBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.Link(uploadPass, vertexBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.Link(uploadPass, indexBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.Link(uploadPass, instanceBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.Link(uploadPass, materialBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.Link(uploadPass, drawArgsBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.Link(uploadPass, lightBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);
    graph.Link(
        uploadPass,
        albedoAtlas,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferWrite,
        RDGTextureLayout::TransferDestination,
        allTexture);

    graph.AddRasterPass(
        "shadow-map-pass",
        make_unique<ShadowRasterPass>(
            sceneBuffer,
            drawArgsBuffer,
            vertexBuffer,
            indexBuffer,
            instanceBuffer,
            albedoAtlas,
            shadowAtlas));
    graph.AddRasterPass(
        "gbuffer-pass",
        make_unique<GBufferRasterPass>(
            sceneBuffer,
            drawArgsBuffer,
            vertexBuffer,
            indexBuffer,
            instanceBuffer,
            materialBuffer,
            albedoAtlas,
            gbufferAlbedo,
            gbufferNormal,
            motionVectors,
            sceneDepth));
    graph.AddComputePass("hiz-build", make_unique<HiZBuildComputePass>(sceneDepth, depthPyramid));
    graph.AddComputePass("ssao-pass", make_unique<SsaoComputePass>(depthPyramid, gbufferNormal, blueNoise, ssaoTexture));
    graph.AddComputePass(
        "light-culling",
        make_unique<LightCullingComputePass>(sceneBuffer, lightBuffer, depthPyramid, lightGrid, visibleLights));
    graph.AddComputePass(
        "deferred-lighting",
        make_unique<DeferredLightingComputePass>(
            sceneBuffer,
            lightGrid,
            visibleLights,
            gbufferAlbedo,
            gbufferNormal,
            sceneDepth,
            shadowAtlas,
            ssaoTexture,
            environmentMap,
            hdrScene,
            luminanceHistogram));
    graph.AddComputePass("exposure-pass", make_unique<ExposureComputePass>(luminanceHistogram, exposureState));
    graph.AddComputePass(
        "taa-resolve",
        make_unique<TaaResolveComputePass>(exposureState, hdrScene, motionVectors, historyColor, taaHdr, historyNext));
    graph.AddComputePass("bloom-prefilter-pass", make_unique<BloomPrefilterComputePass>(taaHdr, bloomPrefilter));
    graph.AddComputePass("bloom-blur-pass", make_unique<BloomBlurComputePass>(bloomPrefilter, bloomBlurred));
    graph.AddComputePass(
        "tone-map-pass",
        make_unique<ToneMapComputePass>(exposureState, taaHdr, bloomBlurred, uiOverlay, finalLdr));

    const auto readbackCopyPass = graph.AddCopyPass("readback-copy");
    graph.Link(
        finalLdr,
        readbackCopyPass,
        RDGExecutionStage::Copy,
        RDGMemoryAccess::TransferRead,
        RDGTextureLayout::TransferSource,
        allTexture);
    graph.Link(readbackCopyPass, readbackBuffer, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, allBuffer);

    graph.ExportBuffer(exposureState, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, allBuffer);
    graph.ExportTexture(
        historyNext,
        RDGExecutionStage::ComputeShader,
        RDGMemoryAccess::ShaderRead,
        RDGTextureLayout::ShaderReadOnly,
        allTexture);
    graph.ExportTexture(
        finalLdr,
        RDGExecutionStage::Host,
        RDGMemoryAccess::HostRead,
        RDGTextureLayout::General,
        allTexture);
    graph.ExportBuffer(readbackBuffer, RDGExecutionStage::Host, RDGMemoryAccess::HostRead, allBuffer);

    const auto graphviz = graph.ExportGraphviz();
    ExpectContains(graphviz, "digraph RenderGraph");
    const auto compiled = graph.Compile();
    const auto dot = compiled.ExportCompiledGraphviz();

    EXPECT_GE(compiled._passes.size(), 10u);
    ExpectContains(dot, "digraph CompiledRenderGraph {");
    ExpectContains(dot, "name: upload-resources");
    ExpectContains(dot, "name: shadow-map-pass");
    ExpectContains(dot, "name: gbuffer-pass");
    ExpectContains(dot, "name: hiz-build");
    ExpectContains(dot, "name: ssao-pass");
    ExpectContains(dot, "name: light-culling");
    ExpectContains(dot, "name: deferred-lighting");
    ExpectContains(dot, "name: exposure-pass");
    ExpectContains(dot, "name: taa-resolve");
    ExpectContains(dot, "name: bloom-prefilter-pass");
    ExpectContains(dot, "name: bloom-blur-pass");
    ExpectContains(dot, "name: tone-map-pass");
    ExpectContains(dot, "name: readback-copy");
    ExpectContains(dot, "name: history-color#0");
    ExpectContains(dot, "name: blue-noise#0");
    ExpectContains(dot, "name: environment-map#0");
    ExpectContains(dot, "name: ui-overlay#0");
    ExpectContains(dot, "name: exposure-state#1");
    ExpectContains(dot, "name: history-next#1");
    ExpectContains(dot, "name: final-ldr#1");
    ExpectContains(dot, "kind: Import");
    ExpectContains(dot, "kind: Export");
    ExpectContains(dot, "access: [TransferRead]");
    ExpectContains(dot, "access: [TransferWrite]");
    ExpectContains(dot, "access: [VertexRead]");
    ExpectContains(dot, "access: [IndirectRead]");
    ExpectContains(dot, "access: [ColorAttachmentWrite]");
    ExpectContains(dot, "access: [DepthStencilWrite]");
    ExpectContains(dot, "access: [ShaderRead | ShaderWrite]");
}

}  // namespace
