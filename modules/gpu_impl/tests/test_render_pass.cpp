#include "gpu_test_fixture.h"

namespace radray::gpu {
namespace {

constexpr uint32_t Size = 4;
constexpr uint32_t DrawPushId = 0;

// Push block of data/graphics.hlsl.
struct DrawData {
    std::array<float, 4> Color;
    float Depth;
};

struct Target {
    unique_ptr<Texture> Image;
    unique_ptr<TextureView> View;
};

class RenderPassTest : public test::BackendTest {
protected:
    Target CreateTarget(TextureFormat format, TextureUses usage, TextureViewUsage viewUsage) {
        auto& device = GetDevice();
        Target target;
        target.Image = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = Size, .Height = Size, .DepthOrArraySize = 1, .MipLevels = 1,
                                             .SampleCount = 1, .Format = format, .Usage = usage})
                           .Unwrap();
        target.View = device.CreateTextureView({target.Image.get(), TextureDimension::Dim2D, format, {0, 1, 0, 1}, viewUsage}).Unwrap();
        return target;
    }

    Target CreateColorTarget() {
        return CreateTarget(TextureFormat::RGBA8_UNORM, TextureUse::RenderTarget | TextureUse::CopySource, TextureViewUsage::RenderTarget);
    }

    struct Pipeline {
        unique_ptr<Shader> VS, PS;
        unique_ptr<PipelineLayout> Layout;
        unique_ptr<GraphicsPipelineState> State;
    };

    Pipeline CreateSolidPipeline(std::optional<DepthStencilState> depth, Nullable<PipelineCache*> cache = nullptr) {
        auto& device = GetDevice();
        Pipeline pipeline;
        pipeline.VS = test::LoadShader(device, "graphics", "FullscreenVS", ShaderStage::Vertex).Unwrap();
        pipeline.PS = test::LoadShader(device, "graphics", "SolidPS", ShaderStage::Pixel).Unwrap();
        const PushConstantDescriptor push{.PushId = DrawPushId, .Size = sizeof(DrawData), .Stages = ShaderStage::Graphics,
                                          .D3D12 = PushConstantDescriptor::D3D12Location{0, 0},
                                          .Vulkan = PushConstantDescriptor::VulkanLocation{0}};
        pipeline.Layout = device.CreatePipelineLayout({{}, std::span{&push, 1}}).Unwrap();
        auto primitive = PrimitiveState::Default();
        primitive.Cull = CullMode::None;
        primitive.UnclippedDepth = false;
        const auto color = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
        pipeline.State = device.CreateGraphicsPipelineState({.PipelineLayout = pipeline.Layout.get(),
                                                             .VS = ShaderEntry{pipeline.VS.get(), "FullscreenVS"},
                                                             .PS = ShaderEntry{pipeline.PS.get(), "SolidPS"},
                                                             .Primitive = primitive,
                                                             .DepthStencil = depth,
                                                             .MultiSample = MultiSampleState::Default(),
                                                             .ColorTargets = std::span{&color, 1}, .Cache = cache})
                             .Unwrap();
        return pipeline;
    }

    static void Draw(GraphicsCommandEncoder& encoder, const DrawData& data) {
        EXPECT_TRUE(encoder.SetPushConstants(DrawPushId, 0, test::AsBytes(data)));
        encoder.Draw(3, 1, 0, 0);
    }

    static void SetFullViewport(GraphicsCommandEncoder& encoder) {
        encoder.SetViewport({0, 0, float(Size), float(Size), 0, 1});
        encoder.SetScissor({0, 0, Size, Size});
    }

    static vector<uint32_t> Fill(uint32_t texel) { return vector<uint32_t>(Size * Size, texel); }

    vector<uint32_t> ReadColor(Texture& texture) {
        return test::FromBytes<uint32_t>(test::ReadTexture(GetDevice(), texture, TextureState::RenderTarget));
    }
};

TEST_P(RenderPassTest, PipelineCacheRoundTripOwnsImportedBytes) {
    auto& device = GetDevice();
    auto cache = device.CreatePipelineCache().Unwrap();
    ASSERT_TRUE(CreateSolidPipeline(std::nullopt, cache.get()).State);
    auto bytes = cache->GetData();
    const bool persisted = !bytes.empty();
    if (GetParam() == GpuBackend::Vulkan) ASSERT_TRUE(persisted);
    cache.reset();
    auto imported = device.CreatePipelineCache(bytes).Unwrap();
    std::fill(bytes.begin(), bytes.end(), byte{0xa7});
    bytes.clear();
    bytes.shrink_to_fit();
    ASSERT_TRUE(CreateSolidPipeline(std::nullopt, imported.get()).State);
    EXPECT_EQ(!imported->GetData().empty(), persisted);
    const array invalid{byte{0x37}, byte{0xff}};
    auto fallback = device.CreatePipelineCache(invalid).Unwrap();
    EXPECT_TRUE(CreateSolidPipeline(std::nullopt, fallback.get()).State);
}

TEST_P(RenderPassTest, ClearColorIsStored) {
    auto& device = GetDevice();
    auto target = CreateColorTarget();

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierTextureDescriptor{.Target = target.Image.get(), .Before = TextureState::Undefined, .After = TextureState::RenderTarget}});
    const RenderPassColorAttachment color{target.View.get(), LoadAction::Clear, StoreAction::Store, {{0.0f, 1.0f, 0.0f, 1.0f}}};
    auto encoder = commands.BeginRenderPass({.Colors = std::span{&color, 1}, .Width = Size, .Height = Size}).Unwrap();
    commands.EndRenderPass(std::move(encoder));
    test::SubmitAndWait(device, recording);

    EXPECT_EQ(ReadColor(*target.Image), Fill(0xff00ff00));
}

TEST_P(RenderPassTest, FullscreenTriangleCoversTarget) {
    auto& device = GetDevice();
    auto target = CreateColorTarget();
    auto pipeline = CreateSolidPipeline(std::nullopt);

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierTextureDescriptor{.Target = target.Image.get(), .Before = TextureState::Undefined, .After = TextureState::RenderTarget}});
    const RenderPassColorAttachment color{target.View.get(), LoadAction::Clear, StoreAction::Store, {{0.0f, 0.0f, 0.0f, 1.0f}}};
    auto encoder = commands.BeginRenderPass({.Colors = std::span{&color, 1}, .Width = Size, .Height = Size}).Unwrap();
    encoder->BindGraphicsPipelineState(pipeline.State.get());
    SetFullViewport(*encoder);
    Draw(*encoder, {{1.0f, 0.0f, 0.0f, 1.0f}, 0.5f});
    commands.EndRenderPass(std::move(encoder));
    test::SubmitAndWait(device, recording);

    EXPECT_EQ(ReadColor(*target.Image), Fill(0xff0000ff));
}

TEST_P(RenderPassTest, DepthTestKeepsNearestFragment) {
    auto& device = GetDevice();
    auto target = CreateColorTarget();
    auto depth = CreateTarget(TextureFormat::D32_FLOAT, TextureUse::DepthStencilWrite, TextureViewUsage::DepthWrite);
    auto depthState = DepthStencilState::Default();
    depthState.Format = TextureFormat::D32_FLOAT;
    auto pipeline = CreateSolidPipeline(depthState);

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierTextureDescriptor{.Target = target.Image.get(), .Before = TextureState::Undefined, .After = TextureState::RenderTarget},
                             BarrierTextureDescriptor{.Target = depth.Image.get(), .Before = TextureState::Undefined, .After = TextureState::DepthWrite}});
    const RenderPassColorAttachment color{target.View.get(), LoadAction::Clear, StoreAction::Store, {{0.0f, 0.0f, 0.0f, 1.0f}}};
    const RenderPassDepthStencilAttachment depthAttachment{.View = depth.View.get(), .DepthLoad = LoadAction::Clear, .DepthStore = StoreAction::Store,
                                                           .Clear = {0.5f, 0}};
    auto encoder = commands.BeginRenderPass({.Colors = std::span{&color, 1}, .DepthStencil = depthAttachment, .Width = Size, .Height = Size}).Unwrap();
    encoder->BindGraphicsPipelineState(pipeline.State.get());
    SetFullViewport(*encoder);
    // Red is behind the cleared depth, green passes and writes depth, blue is behind green.
    Draw(*encoder, {{1.0f, 0.0f, 0.0f, 1.0f}, 0.75f});
    Draw(*encoder, {{0.0f, 1.0f, 0.0f, 1.0f}, 0.25f});
    Draw(*encoder, {{0.0f, 0.0f, 1.0f, 1.0f}, 0.4f});
    commands.EndRenderPass(std::move(encoder));
    test::SubmitAndWait(device, recording);

    EXPECT_EQ(ReadColor(*target.Image), Fill(0xff00ff00));
}

TEST_P(RenderPassTest, CopiedSinglePlaneDepthOccludesOnlyTheNearHalf) {
    auto& device = GetDevice();
    for (auto format : {TextureFormat::D16_UNORM, TextureFormat::D32_FLOAT}) {
        auto color = CreateColorTarget();
        auto source = CreateTarget(format, TextureUse::DepthStencilWrite | TextureUse::CopySource, TextureViewUsage::DepthWrite);
        auto destination = CreateTarget(format, TextureUse::DepthStencilWrite | TextureUse::CopyDestination, TextureViewUsage::DepthWrite);
        auto depthState = DepthStencilState::Default();
        depthState.Format = format;
        auto pipeline = CreateSolidPipeline(depthState);
        auto recording = test::BeginRecording(device);
        auto& commands = *recording.Commands;
        test::Barrier(commands, {BarrierTextureDescriptor{.Target = source.Image.get(), .Before = TextureState::Undefined, .After = TextureState::DepthWrite},
                                 BarrierTextureDescriptor{.Target = destination.Image.get(), .Before = TextureState::Undefined, .After = TextureState::CopyDestination},
                                 BarrierTextureDescriptor{.Target = color.Image.get(), .Before = TextureState::Undefined, .After = TextureState::RenderTarget}});
        RenderPassDepthStencilAttachment depth{.View = source.View.get(), .DepthLoad = LoadAction::Clear, .DepthStore = StoreAction::Store, .Clear = {1.f, 0}};
        auto clear = commands.BeginRenderPass({.DepthStencil = depth, .Width = Size, .Height = Size}).Unwrap();
        commands.EndRenderPass(std::move(clear));
        depth.Clear.Depth = 0.25f;
        clear = commands.BeginRenderPass({.DepthStencil = depth, .Width = Size, .Height = Size, .RenderArea = Rect{0, 0, Size / 2, Size}}).Unwrap();
        commands.EndRenderPass(std::move(clear));
        test::Barrier(commands, {BarrierTextureDescriptor{.Target = source.Image.get(), .Before = TextureState::DepthWrite, .After = TextureState::CopySource}});
        TextureCopyDescriptor copy{.Destination = destination.Image.get(), .Source = source.Image.get(), .Width = Size / 2, .Height = Size};
        commands.CopyTextureToTexture(copy);
        EXPECT_EQ(TakeRhiValidationErrors(), 1u);
        copy.Width = Size;
        commands.CopyTextureToTexture(copy);
        test::Barrier(commands, {BarrierTextureDescriptor{.Target = destination.Image.get(), .Before = TextureState::CopyDestination, .After = TextureState::DepthWrite}});
        depth.View = destination.View.get();
        depth.DepthLoad = LoadAction::Load;
        const RenderPassColorAttachment attachment{color.View.get(), LoadAction::Clear, StoreAction::Store, {{0, 0, 0, 1}}};
        auto encoder = commands.BeginRenderPass({.Colors = {&attachment, 1}, .DepthStencil = depth, .Width = Size, .Height = Size}).Unwrap();
        encoder->BindGraphicsPipelineState(pipeline.State.get());
        SetFullViewport(*encoder);
        Draw(*encoder, {{1, 0, 0, 1}, 0.5f});
        commands.EndRenderPass(std::move(encoder));
        test::SubmitAndWait(device, recording);
        const auto pixels = ReadColor(*color.Image);
        ASSERT_EQ(pixels.size(), Size * Size);
        for (uint32_t y = 0; y < Size; ++y)
            for (uint32_t x = 0; x < Size; ++x)
                EXPECT_EQ(pixels[y * Size + x], x < Size / 2 ? 0xff000000u : 0xff0000ffu);
    }
}

TEST_P(RenderPassTest, RejectsTextureCopiesOfCombinedDepthStencil) {
    auto& device = GetDevice();
    for (auto format : {TextureFormat::D24_UNORM_S8_UINT, TextureFormat::D32_FLOAT_S8_UINT}) {
        const auto usage = TextureUse::DepthStencilWrite | TextureUse::CopySource | TextureUse::CopyDestination;
        if (!device.QueryTextureSupport({TextureDimension::Dim2D, format, usage}).Supported) continue;
        auto a = CreateTarget(format, usage, TextureViewUsage::DepthWrite);
        auto b = CreateTarget(format, usage, TextureViewUsage::DepthWrite);
        auto recording = test::BeginRecording(device);
        recording.Commands->CopyTextureToTexture({.Destination = b.Image.get(), .Source = a.Image.get(), .Width = Size, .Height = Size});
        EXPECT_EQ(TakeRhiValidationErrors(), 1u);
        test::SubmitAndWait(device, recording);
    }
}

TEST_P(RenderPassTest, StencilReferenceIsDynamicAndResetsEachPass) {
    auto& device = GetDevice();
    auto target = CreateColorTarget();
    auto stencil = CreateTarget(TextureFormat::D32_FLOAT_S8_UINT, TextureUse::DepthStencilWrite, TextureViewUsage::DepthWrite);
    auto state = DepthStencilState::Default();
    state.Format = TextureFormat::D32_FLOAT_S8_UINT;
    state.DepthTestEnable = state.DepthWriteEnable = false;
    state.Stencil = StencilState::Default();
    state.Stencil->Front.PassOp = state.Stencil->Back.PassOp = StencilOperation::Replace;
    auto writer = CreateSolidPipeline(state);
    state.Stencil->Front.Compare = state.Stencil->Back.Compare = CompareFunction::Equal;
    state.Stencil->Front.PassOp = state.Stencil->Back.PassOp = StencilOperation::Keep;
    auto tester = CreateSolidPipeline(state);

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierTextureDescriptor{.Target = target.Image.get(), .Before = TextureState::Undefined, .After = TextureState::RenderTarget},
                             BarrierTextureDescriptor{.Target = stencil.Image.get(), .Before = TextureState::Undefined, .After = TextureState::DepthWrite}});
    RenderPassColorAttachment color{target.View.get(), LoadAction::Clear, StoreAction::Store, {{0.0f, 0.0f, 0.0f, 1.0f}}};
    RenderPassDepthStencilAttachment stencilAttachment{.View = stencil.View.get(), .DepthLoad = LoadAction::Clear, .DepthStore = StoreAction::Store,
                                                       .StencilLoad = LoadAction::Clear, .StencilStore = StoreAction::Store, .Clear = {1.0f, 0}};
    auto encoder = commands.BeginRenderPass({.Colors = std::span{&color, 1}, .DepthStencil = stencilAttachment, .Width = Size, .Height = Size}).Unwrap();
    SetFullViewport(*encoder);
    // Writes stencil 7 to the left half only; the right half keeps the cleared 0.
    encoder->SetStencilReference(7);
    encoder->BindGraphicsPipelineState(writer.State.get());
    encoder->SetScissor({0, 0, Size / 2, Size});
    Draw(*encoder, {{1.0f, 0.0f, 0.0f, 1.0f}, 0.5f});
    // Switching pipelines keeps the reference, so green passes on the left.
    encoder->SetScissor({0, 0, Size, Size});
    encoder->BindGraphicsPipelineState(tester.State.get());
    Draw(*encoder, {{0.0f, 1.0f, 0.0f, 1.0f}, 0.5f});
    encoder->SetStencilReference(3);
    Draw(*encoder, {{0.0f, 0.0f, 1.0f, 1.0f}, 0.5f});
    commands.EndRenderPass(std::move(encoder));

    test::Barrier(commands, {BarrierTextureDescriptor{.Target = target.Image.get(), .Before = TextureState::RenderTarget, .After = TextureState::RenderTarget},
                             BarrierTextureDescriptor{.Target = stencil.Image.get(), .Before = TextureState::DepthWrite, .After = TextureState::DepthWrite}});
    color.Load = LoadAction::Load;
    stencilAttachment.DepthLoad = stencilAttachment.StencilLoad = LoadAction::Load;
    encoder = commands.BeginRenderPass({.Colors = std::span{&color, 1}, .DepthStencil = stencilAttachment, .Width = Size, .Height = Size}).Unwrap();
    SetFullViewport(*encoder);
    // A new pass starts with reference 0, which matches only the right half.
    encoder->BindGraphicsPipelineState(tester.State.get());
    Draw(*encoder, {{1.0f, 1.0f, 1.0f, 1.0f}, 0.5f});
    commands.EndRenderPass(std::move(encoder));
    test::SubmitAndWait(device, recording);

    vector<uint32_t> expected(Size * Size);
    for (uint32_t y = 0; y < Size; ++y) {
        for (uint32_t x = 0; x < Size; ++x) expected[y * Size + x] = x < Size / 2 ? 0xff00ff00 : 0xffffffff;
    }
    EXPECT_EQ(ReadColor(*target.Image), expected);
}

INSTANTIATE_TEST_SUITE_P(Backends, RenderPassTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::gpu
