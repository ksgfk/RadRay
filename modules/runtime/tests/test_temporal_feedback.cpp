#include "foundation_graph_fixture.h"
#include <fstream>
#include <cmath>

namespace radray {
namespace {
class TemporalFeedbackTest : public test::FoundationGraphGpuTest {};

TEST_P(TemporalFeedbackTest, FeedbackPersistsAcrossGraphsAndSupportsPauseAndReset) {
    std::ifstream file{std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/runtime/tests/data/temporal_feedback.hlsl"};
    ASSERT_TRUE(file);
    const string source{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    auto program = test::CompileFoundationCompute(*Context.Device, source);
    ASSERT_TRUE(program);
    constexpr uint32_t width = 128, height = 80;
    array<unique_ptr<render::Texture>, 2> textures;
    array<array<render::TextureStates, 1>, 2> states{{{render::TextureState::Undefined}, {render::TextureState::Undefined}}};
    array<array<uint8_t, 1>, 2> valid{};
    array<RenderExternalTexture, 2> imports{};
    for (size_t i = 0; i < textures.size(); ++i) {
        auto texture = Context.Device->CreateTexture({render::TextureDimension::Dim2D, width, height, 1, 1, 1,
                                                      render::TextureFormat::RGBA32_FLOAT, render::MemoryType::Device,
                                                      render::TextureUse::UnorderedAccess | render::TextureUse::Resource | render::TextureUse::CopySource});
        ASSERT_TRUE(texture);
        textures[i] = texture.Release();
        imports[i] = {textures[i].get(), textures[i]->GetDesc(), states[i], valid[i]};
    }
    const auto pitch = Align(uint64_t{width} * 16, Context.Device->GetDetail().TextureDataPitchAlignment);
    auto readback = Context.Device->CreateBuffer({pitch * height, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead});
    ASSERT_TRUE(readback);
    RenderExternalBuffer host{readback.Get(), readback->GetDesc(), render::BufferState::CopyDestination};
    array<vector<float>, 4> frames;
    const array<Eigen::Vector4f, 4> parameters{{{0, 0, 0, 0}, {1, 1, 1, 1}, {2, 1, 0, 1}, {2, 0, 0, 0}}};
    for (uint32_t frame = 0; frame < 4; ++frame) {
        Resources->BeginFlight(frame + 2, Writes);
        auto graph = MakeGraph("temporal feedback");
        const auto previous = frame == 0 ? graph.CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1,
                                                                render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource},
                                                               "initial black")
                                         : graph.ImportTexture(imports[(frame + 1) % 2], "previous feedback", RenderGraphExternalAccess::ReadOnly);
        const auto current = graph.ImportTexture(imports[frame % 2], "current feedback", RenderGraphExternalAccess::ObservableOutput);
        if (frame == 0) graph.AddRasterPass<int>("initialize history", [&](int&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, previous); }, nullptr);
        ShaderParameterStorage values{&program->GetParameterLayout(), 0};
        ASSERT_TRUE(values.SetFloat4("SignalFrame.State", parameters[frame]));
        render::SamplerDescriptor sampler;
        sampler.AddressS = sampler.AddressT = render::AddressMode::ClampToEdge;
        sampler.MinFilter = sampler.MagFilter = render::FilterMode::Linear;
        struct Data {
            RgComputeProgramHandle Program;
            RgParameterSetHandle Frame, Resources;
        };
        graph.AddComputePass<Data>("feedback", [&](Data& data, RenderGraphComputeBuilder& builder) {
            const RgParameterBinding constants[]{{"SignalFrame", 0, RgCBufferParameterBinding{values.GetBufferData(0)}}};
            const RgParameterBinding resources[]{
                {"SignalOutput", 0, RgTextureParameterBinding{current, {}, RgParameterAccess::Write}},
                {"SignalPrevious", 0, RgTextureParameterBinding{previous}},
                {"SignalSampler", 0, RgSamplerParameterBinding{sampler}}};
            data = {builder.UseComputeProgram(*program.Get()), builder.CreateParameterSet(*program.Get(), 0, constants), builder.CreateParameterSet(*program.Get(), 1, resources)}; }, +[](const Data& data, RenderGraphComputeContext& ctx) {
            ctx.BindComputeProgram(data.Program);
            ctx.BindParameterSet(data.Frame);
            ctx.BindParameterSet(data.Resources);
            ctx.Encoder().Dispatch(width / 8, height / 8, 1); });
        const auto destination = graph.ImportBuffer(host, "feedback readback", RenderGraphExternalAccess::ObservableOutput);
        graph.AddCopyTextureToBufferPass("read feedback", current, destination);
        HostRead(graph, destination);
        ASSERT_TRUE(Run(graph)) << graph.GetReport().ToText();
        const auto bytes = Read(*readback.Get());
        ASSERT_EQ(bytes.size(), pitch * height);
        frames[frame].resize(width * height * 4);
        for (uint32_t y = 0; y < height; ++y) std::memcpy(frames[frame].data() + y * width * 4, bytes.data() + y * pitch, width * 16);
    }
    size_t accumulated = 0, animated = 0;
    for (size_t i = 0; i < frames[0].size(); ++i) {
        for (const auto& frame : frames) {
            ASSERT_TRUE(std::isfinite(frame[i]));
            EXPECT_GE(frame[i], 0);
            EXPECT_LE(frame[i], 1);
        }
        EXPECT_NEAR(frames[0][i], frames[1][i], 1e-5f);
        EXPECT_GE(frames[2][i] + 1e-5f, frames[3][i]);
        accumulated += frames[2][i] > frames[3][i] + .01f;
        animated += std::abs(frames[3][i] - frames[0][i]) > .01f;
        if (i % 4 == 3) EXPECT_FLOAT_EQ(frames[2][i], 1);
    }
    EXPECT_GT(accumulated, 100u);
    EXPECT_GT(animated, 100u);
}

INSTANTIATE_TEST_SUITE_P(Backends, TemporalFeedbackTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));
}  // namespace
}  // namespace radray
