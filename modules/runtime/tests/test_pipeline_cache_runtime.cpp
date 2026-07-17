#include "render_test_framework.h"

#include <filesystem>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <radray/file.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/pipeline_cache.h>
#include <radray/runtime/shader_asset.h>

namespace radray::render::test {
namespace {

class PipelineCacheRuntimeTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        string reason;
        if (!_ctx.Initialize(GetParam(), &reason)) {
            GTEST_SKIP() << fmt::format("Init failed on {}: {}", format_as(GetParam()), reason);
        }
        _cacheDirectory = std::filesystem::temp_directory_path() /
                          fmt::format("radray_native_pipeline_cache_{}", Guid::NewGuid());
    }

    void TearDown() override {
        _ctx.Reset();
        std::error_code ignored;
        std::filesystem::remove_all(_cacheDirectory, ignored);
    }

    bool Reinitialize(string* reason) {
        _ctx.Reset();
        return _ctx.Initialize(GetParam(), reason);
    }

    ComputeTestContext _ctx{};
    std::filesystem::path _cacheDirectory{};
};

TEST_P(PipelineCacheRuntimeTest, RecreatesGraphicsPipelineAcrossDeviceLifetimes) {
    const std::filesystem::path shaderRoot = GetExecutableDirectory() / "shaderlib";
    if (!std::filesystem::is_regular_file(shaderRoot / "forward_pipeline/error_pass.hlsl")) {
        GTEST_SKIP() << "deployed shaderlib is unavailable";
    }

    const AssetId shaderId = Guid::NewGuid();
    const ShaderModuleKey vertexKey{
        .Shader = shaderId,
        .PassIndex = 0,
        .Stage = ShaderStage::Vertex};
    const ShaderModuleKey pixelKey{
        .Shader = shaderId,
        .PassIndex = 0,
        .Stage = ShaderStage::Pixel};

    {
        AssetManager assets;
        ShaderPassDesc pass{};
        pass.Name = "NativePipelineCacheTest";
        pass.SourcePath = "forward_pipeline/error_pass.hlsl";
        pass.SM = HlslShaderModel::SM65;
        pass.IsOptimize = false;
        ShaderGraphicsPassDesc& graphics = std::get<ShaderGraphicsPassDesc>(pass.Program);
        graphics.VertexEntry = "VSMain";
        graphics.PixelEntry = "PSMain";
        StreamingAssetRef<ShaderAsset> shaderAsset = assets.AddReady(
            shaderId,
            make_unique<ShaderAsset>(vector<ShaderPassDesc>{std::move(pass)}));
        ASSERT_TRUE(shaderAsset.IsReady());

        ShaderModuleCache shaderCache{
            _ctx.GetDevicePtr(),
            _ctx.GetDxc(),
            &assets,
            shaderRoot.string(),
            _cacheDirectory};
        Shader* vertexShader = shaderCache.GetOrCreate(vertexKey).Get();
        Shader* pixelShader = shaderCache.GetOrCreate(pixelKey).Get();
        ASSERT_NE(vertexShader, nullptr);
        ASSERT_NE(pixelShader, nullptr);

        GraphicsPipelineCache pipelineCache{
            _ctx.GetDevicePtr(),
            &shaderCache,
            _cacheDirectory};
        vector<Shader*> shaders{vertexShader, pixelShader};
        PipelineLayout* layout = pipelineCache.GetOrCreatePipelineLayout(
            PipelineLayoutDescriptor{.Shaders = shaders})
                                     .Get();
        ASSERT_NE(layout, nullptr);

        vector<RenderPassColorAttachmentDescriptor> passColors{
            {.Format = TextureFormat::RGBA8_UNORM, .SampleCount = 1}};
        auto renderPassOpt = _ctx.GetDevicePtr()->CreateRenderPass(
            RenderPassDescriptor{.ColorAttachments = passColors});
        ASSERT_TRUE(renderPassOpt.HasValue());
        unique_ptr<RenderPass> renderPass = renderPassOpt.Release();
        vector<VertexElement> elements{{
            .Offset = 0,
            .Semantic = "POSITION",
            .SemanticIndex = 0,
            .Format = VertexFormat::FLOAT32X3,
            .Location = 0}};
        vector<VertexBufferLayout> vertexLayouts{{
            .ArrayStride = 12,
            .StepMode = VertexStepMode::Vertex,
            .Elements = elements}};
        vector<ColorTargetState> colorTargets{
            ColorTargetState::Default(TextureFormat::RGBA8_UNORM)};
        auto pso = pipelineCache.GetOrCreateGraphicsPso(GraphicsPipelineStateDescriptor{
            .PipelineLayout = layout,
            .VS = ShaderEntry{.Target = vertexShader, .EntryPoint = "VSMain"},
            .PS = ShaderEntry{.Target = pixelShader, .EntryPoint = "PSMain"},
            .VertexLayouts = vertexLayouts,
            .Primitive = PrimitiveState::Default(),
            .MultiSample = MultiSampleState::Default(),
            .ColorTargets = colorTargets,
            .CompatibleRenderPass = renderPass.get()});
        ASSERT_TRUE(pso.HasValue()) << _ctx.JoinCapturedErrors();
        ASSERT_TRUE(shaderCache.FlushToDisk());
        ASSERT_TRUE(pipelineCache.FlushToDisk());
        renderPass->Destroy();
    }

    string reason;
    ASSERT_TRUE(Reinitialize(&reason)) << reason;
    _ctx.ClearCapturedErrors();

    {
        ShaderModuleCache shaderCache{
            _ctx.GetDevicePtr(),
            nullptr,
            nullptr,
            {},
            _cacheDirectory};
        Shader* vertexShader = shaderCache.GetOrCreate(vertexKey).Get();
        Shader* pixelShader = shaderCache.GetOrCreate(pixelKey).Get();
        ASSERT_NE(vertexShader, nullptr);
        ASSERT_NE(pixelShader, nullptr);

        GraphicsPipelineCache pipelineCache{
            _ctx.GetDevicePtr(),
            &shaderCache,
            _cacheDirectory};
        EXPECT_EQ(pipelineCache.GetPipelineLayoutCount(), 1u);
        EXPECT_EQ(pipelineCache.GetGraphicsPsoCount(), 1u);
        vector<Shader*> shaders{vertexShader, pixelShader};
        PipelineLayout* layout = pipelineCache.GetOrCreatePipelineLayout(
            PipelineLayoutDescriptor{.Shaders = shaders})
                                     .Get();
        ASSERT_NE(layout, nullptr);

        vector<RenderPassColorAttachmentDescriptor> passColors{
            {.Format = TextureFormat::RGBA8_UNORM, .SampleCount = 1}};
        auto renderPassOpt = _ctx.GetDevicePtr()->CreateRenderPass(
            RenderPassDescriptor{.ColorAttachments = passColors});
        ASSERT_TRUE(renderPassOpt.HasValue());
        unique_ptr<RenderPass> renderPass = renderPassOpt.Release();
        vector<VertexElement> elements{{
            .Offset = 0,
            .Semantic = "POSITION",
            .SemanticIndex = 0,
            .Format = VertexFormat::FLOAT32X3,
            .Location = 0}};
        vector<VertexBufferLayout> vertexLayouts{{
            .ArrayStride = 12,
            .StepMode = VertexStepMode::Vertex,
            .Elements = elements}};
        vector<ColorTargetState> colorTargets{
            ColorTargetState::Default(TextureFormat::RGBA8_UNORM)};
        auto pso = pipelineCache.GetOrCreateGraphicsPso(GraphicsPipelineStateDescriptor{
            .PipelineLayout = layout,
            .VS = ShaderEntry{.Target = vertexShader, .EntryPoint = "VSMain"},
            .PS = ShaderEntry{.Target = pixelShader, .EntryPoint = "PSMain"},
            .VertexLayouts = vertexLayouts,
            .Primitive = PrimitiveState::Default(),
            .MultiSample = MultiSampleState::Default(),
            .ColorTargets = colorTargets,
            .CompatibleRenderPass = renderPass.get()});
        ASSERT_TRUE(pso.HasValue()) << _ctx.JoinCapturedErrors();
        EXPECT_TRUE(_ctx.GetCapturedErrors().empty()) << _ctx.JoinCapturedErrors();
        renderPass->Destroy();
    }
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    PipelineCacheRuntimeTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{format_as(info.param)};
    });

}  // namespace
}  // namespace radray::render::test
