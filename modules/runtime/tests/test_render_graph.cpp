#include <gtest/gtest.h>

#include <radray/runtime/render_graph.h>

using namespace radray;
using namespace radray::render;

namespace {

RGBufferDesc MakeBufferDesc() {
    RGBufferDesc desc{};
    desc.Size = 256;
    desc.Memory = MemoryType::Device;
    desc.Hints = ResourceHint::None;
    return desc;
}

RGTextureDesc MakeTextureDesc() {
    RGTextureDesc desc{};
    desc.Dim = TextureDimension::Dim2D;
    desc.Width = 16;
    desc.Height = 16;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleCount = 1;
    desc.Format = TextureFormat::RGBA8_UNORM;
    desc.Memory = MemoryType::Device;
    desc.Hints = ResourceHint::None;
    return desc;
}

RGAccelerationStructureDesc MakeAccelerationStructureDesc() {
    RGAccelerationStructureDesc desc{};
    desc.Type = AccelerationStructureType::TopLevel;
    desc.MaxInstanceCount = 8;
    desc.Flags = AccelerationStructureBuildFlag::PreferFastTrace;
    return desc;
}

bool HasIssue(const RGValidationResult& result, RGValidationIssueCode code) {
    for (const auto& issue : result.Issues) {
        if (issue.Code == code) {
            return true;
        }
    }
    return false;
}

size_t CountIssues(const RGValidationResult& result, RGValidationIssueCode code) {
    size_t count = 0;
    for (const auto& issue : result.Issues) {
        if (issue.Code == code) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST(RenderGraph, ValidGraphProducesStableGraphvizAndValidationSuccess) {
    RenderGraph graph{};

    const auto vertexBuffer = graph.AddBuffer("vertex-buffer", MakeBufferDesc());
    const auto colorTexture = graph.AddTexture("color-target", MakeTextureDesc());

    auto pass = graph.AddGraphicsPass("main-pass");
    pass.ReadVertexBuffer(vertexBuffer, {0, 64}, "vb");
    pass.BindColorAttachment(colorTexture, {.Slot = 0}, "color");

    graph.ExportTexture(colorTexture, {
        .Queue = QueueType::Direct,
        .State = TextureState::Present,
        .Range = RGWholeSubresourceRange()});

    const string dot1 = graph.ToGraphviz();
    const string dot2 = graph.ToGraphviz();

    EXPECT_EQ(dot1, dot2);
    EXPECT_NE(dot1.find("digraph RenderGraph"), string::npos);
    EXPECT_NE(dot1.find("vertex-buffer"), string::npos);
    EXPECT_NE(dot1.find("vertex-buffer@v0"), string::npos);
    EXPECT_NE(dot1.find("main-pass"), string::npos);
    EXPECT_NE(dot1.find("ColorAttachmentWrite"), string::npos);
    EXPECT_NE(dot1.find("color-target@v1"), string::npos);
    EXPECT_NE(dot1.find("Export Texture"), string::npos);
    EXPECT_NE(dot1.find("fillcolor=\"#bde0fe\""), string::npos);

    const auto validation = graph.Validate();
    EXPECT_TRUE(validation.IsValid());
    EXPECT_TRUE(validation.Issues.empty());
}

TEST(RenderGraph, MissingExternalHandleAndDuplicateExportAreValidatedSeparately) {
    RenderGraph graph{};

    RGImportedTextureDesc imported{};
    imported.External = GpuResourceHandle::Invalid();
    imported.Desc = MakeTextureDesc();
    imported.InitialState = {
        .Queue = QueueType::Direct,
        .State = TextureState::Common,
        .Range = RGWholeSubresourceRange()};

    const auto texture = graph.ImportTexture("swapchain-image", imported);

    graph.ExportTexture(texture, {
        .Queue = QueueType::Direct,
        .State = TextureState::Present,
        .Range = RGWholeSubresourceRange()});
    graph.ExportTexture(texture, {
        .Queue = QueueType::Direct,
        .State = TextureState::Present,
        .Range = RGWholeSubresourceRange()});

    const auto validation = graph.Validate();
    EXPECT_FALSE(validation.IsValid());
    EXPECT_TRUE(HasIssue(validation, RGValidationIssueCode::MissingImportedExternalHandle));
    EXPECT_TRUE(HasIssue(validation, RGValidationIssueCode::DuplicateExport));
}

TEST(RenderGraph, InvalidHandlesAreRecordedAndShownInGraphviz) {
    RenderGraph graph{};

    auto pass = graph.AddComputePass("compute");
    pass.ReadStorageBuffer(RGResourceHandle::Invalid(), {}, "bad-buffer");
    graph.ExportBuffer(RGResourceHandle::Invalid(), {
        .Queue = QueueType::Direct,
        .State = BufferState::Common,
        .Range = RGWholeBufferRange()});

    const auto validation = graph.Validate();
    EXPECT_FALSE(validation.IsValid());
    EXPECT_GE(CountIssues(validation, RGValidationIssueCode::MissingResource), 2u);

    const auto dot = graph.ToGraphviz();
    EXPECT_NE(dot.find("Invalid Resource"), string::npos);
    EXPECT_NE(dot.find("StorageBufferRead"), string::npos);
}

TEST(RenderGraph, InvalidExportKindIsReportedByValidation) {
    RenderGraph graph{};

    const auto buffer = graph.AddBuffer("buffer", MakeBufferDesc());
    graph.ExportTexture(buffer, {
        .Queue = QueueType::Direct,
        .State = TextureState::Present,
        .Range = RGWholeSubresourceRange()});

    const auto validation = graph.Validate();
    EXPECT_FALSE(validation.IsValid());
    EXPECT_TRUE(HasIssue(validation, RGValidationIssueCode::InvalidExportForKind));
}

TEST(RenderGraph, CopyAndRayTracingRelationshipsAreTracked) {
    RenderGraph graph{};

    const auto upload = graph.AddBuffer("upload", MakeBufferDesc());
    const auto scratch = graph.AddBuffer("scratch", MakeBufferDesc());
    const auto texture = graph.AddTexture("texture", MakeTextureDesc());
    const auto accel = graph.AddAccelerationStructure("tlas", MakeAccelerationStructureDesc());

    auto copy = graph.AddCopyPass("copy-pass");
    copy.CopyBufferToTexture(upload, {0, 128}, texture, RGWholeSubresourceRange(), "upload->texture");

    auto rayTracing = graph.AddRayTracingPass("rt-pass");
    rayTracing.ReadShaderTableBuffer(upload, {0, 64}, "sbt");
    rayTracing.BuildAccelerationStructure(accel, {
        .ScratchBuffer = scratch,
        .ScratchRange = {0, 128},
        .Mode = AccelerationStructureBuildMode::Build}, "build-tlas");

    const auto validation = graph.Validate();
    EXPECT_TRUE(validation.IsValid());

    const auto dot = graph.ToGraphviz();
    EXPECT_NE(dot.find("CopyTextureWrite"), string::npos);
    EXPECT_NE(dot.find("ShaderTableRead"), string::npos);
    EXPECT_NE(dot.find("AccelerationStructureBuild"), string::npos);
    EXPECT_NE(dot.find("BuildScratchBuffer"), string::npos);
    EXPECT_NE(dot.find("texture@v1"), string::npos);
    EXPECT_NE(dot.find("scratch@v1"), string::npos);
}

TEST(RenderGraph, GraphvizUsesVersionedResourceNodesAndStableColors) {
    RenderGraph graph{};

    const auto history = graph.AddTexture("history", MakeTextureDesc());

    auto writeA = graph.AddComputePass("write-a");
    writeA.ReadWriteStorageTexture(history, {}, "history-a");

    auto writeB = graph.AddComputePass("write-b");
    writeB.ReadSampledTexture(history, {}, "history-read");
    writeB.ReadWriteStorageTexture(history, {}, "history-b");

    const auto dot = graph.ToGraphviz();

    EXPECT_NE(dot.find("history@v0"), string::npos);
    EXPECT_NE(dot.find("history@v1"), string::npos);
    EXPECT_NE(dot.find("history@v2"), string::npos);
    EXPECT_NE(dot.find("res_0_v0 -> pass_0"), string::npos);
    EXPECT_NE(dot.find("pass_0 -> res_0_v1"), string::npos);
    EXPECT_NE(dot.find("res_0_v1 -> pass_1"), string::npos);
    EXPECT_NE(dot.find("pass_1 -> res_0_v2"), string::npos);
    EXPECT_NE(dot.find("fillcolor=\"#f4a261\""), string::npos);
}

TEST(RenderGraph, ComplexFrameGraphScenarioValidatesAndExportsGraphviz) {
    RenderGraph graph{};

    const auto sceneConstants = graph.AddBuffer("scene-constants", MakeBufferDesc());
    const auto instanceData = graph.AddBuffer("instance-data", MakeBufferDesc());
    const auto lightGrid = graph.AddBuffer("light-grid", MakeBufferDesc());
    const auto transparentList = graph.AddBuffer("transparent-list", MakeBufferDesc());
    const auto tileClassification = graph.AddBuffer("tile-classification", MakeBufferDesc());
    const auto exposureHistogram = graph.AddBuffer("exposure-histogram", MakeBufferDesc());

    const auto depth = graph.AddTexture("scene-depth", MakeTextureDesc());
    const auto hiz = graph.AddTexture("scene-hiz", MakeTextureDesc());
    const auto gbufferAlbedo = graph.AddTexture("gbuffer-albedo", MakeTextureDesc());
    const auto gbufferNormal = graph.AddTexture("gbuffer-normal", MakeTextureDesc());
    const auto gbufferMaterial = graph.AddTexture("gbuffer-material", MakeTextureDesc());
    const auto hdrLight = graph.AddTexture("hdr-lighting", MakeTextureDesc());
    const auto volumetric = graph.AddTexture("volumetric-lighting", MakeTextureDesc());
    const auto transparentColor = graph.AddTexture("transparent-color", MakeTextureDesc());
    const auto postFx = graph.AddTexture("postfx-color", MakeTextureDesc());
    const auto bloomPrefilter = graph.AddTexture("bloom-prefilter", MakeTextureDesc());
    const auto bloomBlurA = graph.AddTexture("bloom-blur-a", MakeTextureDesc());
    const auto bloomBlurB = graph.AddTexture("bloom-blur-b", MakeTextureDesc());
    const auto ssrResolve = graph.AddTexture("ssr-resolve", MakeTextureDesc());
    const auto taaHistory = graph.AddTexture("taa-history", MakeTextureDesc());
    const auto finalColor = graph.AddTexture("final-color", MakeTextureDesc());

    auto earlyZ = graph.AddGraphicsPass("early-z");
    earlyZ.ReadConstantBuffer(sceneConstants, {}, "scene-cb");
    earlyZ.ReadVertexBuffer(instanceData, {0, 128}, "depth-vertices");
    earlyZ.BindDepthStencilAttachment(depth, {}, "depth-prepass");

    auto gbuffer = graph.AddGraphicsPass("gbuffer");
    gbuffer.ReadConstantBuffer(sceneConstants, {}, "scene-cb");
    gbuffer.ReadVertexBuffer(instanceData, {0, 128}, "gbuffer-vertices");
    gbuffer.BindColorAttachment(gbufferAlbedo, {.Slot = 0}, "albedo");
    gbuffer.BindColorAttachment(gbufferNormal, {.Slot = 1}, "normal");
    gbuffer.BindColorAttachment(gbufferMaterial, {.Slot = 2}, "material");
    gbuffer.BindDepthStencilAttachment(depth, {}, "depth-write");

    auto buildHiz = graph.AddComputePass("build-hiz");
    buildHiz.ReadSampledTexture(depth, {}, "depth-pyramid-src");
    buildHiz.ReadWriteStorageTexture(hiz, {}, "hiz-pyramid");

    auto lighting = graph.AddComputePass("deferred-lighting");
    lighting.ReadConstantBuffer(sceneConstants, {}, "scene-cb");
    lighting.ReadStorageBuffer(lightGrid, {}, "light-grid");
    lighting.ReadSampledTexture(gbufferAlbedo, {}, "gbuffer-albedo");
    lighting.ReadSampledTexture(gbufferNormal, {}, "gbuffer-normal");
    lighting.ReadSampledTexture(gbufferMaterial, {}, "gbuffer-material");
    lighting.ReadSampledTexture(depth, {}, "scene-depth");
    lighting.ReadSampledTexture(hiz, {}, "scene-hiz");
    lighting.ReadWriteStorageBuffer(tileClassification, {}, "light-tiles");
    lighting.ReadWriteStorageTexture(hdrLight, {}, "hdr-lighting");

    auto volumetricPass = graph.AddComputePass("volumetric-fog");
    volumetricPass.ReadConstantBuffer(sceneConstants, {}, "scene-cb");
    volumetricPass.ReadSampledTexture(depth, {}, "scene-depth");
    volumetricPass.ReadSampledTexture(hiz, {}, "scene-hiz");
    volumetricPass.ReadWriteStorageTexture(volumetric, {}, "volumetric-out");

    auto ssrTrace = graph.AddComputePass("ssr-trace");
    ssrTrace.ReadSampledTexture(hiz, {}, "scene-hiz");
    ssrTrace.ReadSampledTexture(gbufferNormal, {}, "gbuffer-normal");
    ssrTrace.ReadSampledTexture(hdrLight, {}, "hdr-lighting");
    ssrTrace.ReadWriteStorageTexture(ssrResolve, {}, "ssr-resolve");

    auto exposure = graph.AddComputePass("auto-exposure");
    exposure.ReadSampledTexture(hdrLight, {}, "hdr-lighting");
    exposure.ReadSampledTexture(ssrResolve, {}, "ssr-resolve");
    exposure.ReadWriteStorageBuffer(exposureHistogram, {}, "exposure-histogram");

    auto transparency = graph.AddGraphicsPass("transparency");
    transparency.ReadConstantBuffer(sceneConstants, {}, "scene-cb");
    transparency.ReadStorageBuffer(transparentList, {}, "transparent-list");
    transparency.ReadSampledTexture(depth, {}, "scene-depth");
    transparency.BindColorAttachment(transparentColor, {.Slot = 0}, "transparent-color");

    auto composite = graph.AddGraphicsPass("composite");
    composite.ReadSampledTexture(hdrLight, {}, "hdr-lighting");
    composite.ReadSampledTexture(volumetric, {}, "volumetric-lighting");
    composite.ReadSampledTexture(ssrResolve, {}, "ssr-resolve");
    composite.ReadSampledTexture(transparentColor, {}, "transparent-color");
    composite.BindColorAttachment(postFx, {.Slot = 0}, "postfx-out");

    auto taaResolve = graph.AddComputePass("taa-resolve");
    taaResolve.ReadSampledTexture(postFx, {}, "postfx-in");
    taaResolve.ReadStorageBuffer(exposureHistogram, {}, "exposure-histogram");
    taaResolve.ReadWriteStorageTexture(taaHistory, {}, "taa-history");

    auto bloomDownsample = graph.AddComputePass("bloom-downsample");
    bloomDownsample.ReadSampledTexture(taaHistory, {}, "taa-history");
    bloomDownsample.ReadWriteStorageTexture(bloomPrefilter, {}, "bloom-prefilter");

    auto bloomBlurHorizontal = graph.AddComputePass("bloom-blur-horizontal");
    bloomBlurHorizontal.ReadSampledTexture(bloomPrefilter, {}, "bloom-prefilter");
    bloomBlurHorizontal.ReadWriteStorageTexture(bloomBlurA, {}, "bloom-a");

    auto bloomBlurVertical = graph.AddComputePass("bloom-blur-vertical");
    bloomBlurVertical.ReadSampledTexture(bloomBlurA, {}, "bloom-a");
    bloomBlurVertical.ReadWriteStorageTexture(bloomBlurB, {}, "bloom-b");

    auto tonemap = graph.AddGraphicsPass("tonemap");
    tonemap.ReadSampledTexture(taaHistory, {}, "postfx-color");
    tonemap.ReadSampledTexture(bloomBlurB, {}, "bloom-color");
    tonemap.BindColorAttachment(finalColor, {.Slot = 0}, "final-color");

    graph.ExportTexture(finalColor, {
        .Queue = QueueType::Direct,
        .State = TextureState::Present,
        .Range = RGWholeSubresourceRange()});

    const auto validation = graph.Validate();
    EXPECT_TRUE(validation.IsValid());
    EXPECT_TRUE(validation.Issues.empty());

    const auto dot = graph.ToGraphviz();
    EXPECT_NE(dot.find("early-z"), string::npos);
    EXPECT_NE(dot.find("gbuffer"), string::npos);
    EXPECT_NE(dot.find("build-hiz"), string::npos);
    EXPECT_NE(dot.find("deferred-lighting"), string::npos);
    EXPECT_NE(dot.find("volumetric-fog"), string::npos);
    EXPECT_NE(dot.find("ssr-trace"), string::npos);
    EXPECT_NE(dot.find("auto-exposure"), string::npos);
    EXPECT_NE(dot.find("transparency"), string::npos);
    EXPECT_NE(dot.find("composite"), string::npos);
    EXPECT_NE(dot.find("taa-resolve"), string::npos);
    EXPECT_NE(dot.find("bloom-downsample"), string::npos);
    EXPECT_NE(dot.find("bloom-blur-horizontal"), string::npos);
    EXPECT_NE(dot.find("bloom-blur-vertical"), string::npos);
    EXPECT_NE(dot.find("tonemap"), string::npos);
    EXPECT_NE(dot.find("scene-hiz"), string::npos);
    EXPECT_NE(dot.find("tile-classification"), string::npos);
    EXPECT_NE(dot.find("exposure-histogram"), string::npos);
    EXPECT_NE(dot.find("ssr-resolve"), string::npos);
    EXPECT_NE(dot.find("taa-history"), string::npos);
    EXPECT_NE(dot.find("StorageBufferReadWrite"), string::npos);
    EXPECT_NE(dot.find("ColorAttachmentWrite"), string::npos);
    EXPECT_NE(dot.find("StorageTextureReadWrite"), string::npos);
    EXPECT_NE(dot.find("Export Texture"), string::npos);
}

TEST(RenderGraph, CompileIsNoOpAndExecuteThrowsNotImplemented) {
    RenderGraph graph{};
    const auto compiled = graph.Compile();

    RGExecutionEnvironment environment{};
    EXPECT_THROW(graph.Execute(compiled, environment), RenderGraphException);
}
