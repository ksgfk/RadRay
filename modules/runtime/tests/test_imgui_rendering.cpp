#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include <cstring>
#include <gtest/gtest.h>
#include <imgui_internal.h>
#include <radray/runtime/imgui/imgui_graph.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {
struct UiTestMode {
    render::RenderBackend Backend;
    bool Threaded;
    bool Srgb;
    uint32_t Flights;
};
void PrintTo(const UiTestMode& mode, std::ostream* output) {
    *output << (mode.Backend == render::RenderBackend::D3D12 ? "D3D12" : "Vulkan") << (mode.Threaded ? "Threaded" : "Single")
            << (mode.Srgb ? "Srgb" : "Unorm") << mode.Flights;
}
class UiProbePipeline final : public RenderPipeline {
public:
    struct Flight {
        unique_ptr<render::Buffer> Buffer;
        RenderExternalBuffer Import{nullptr, {}, render::BufferState::CopyDestination};
        uint64_t Pitch{0};
        uint32_t Height{0}, Stage{0}, Frame{0};
        bool Pending{false}, Bgra{false};
    };
    UiProbePipeline(Application& app, ImTextureID image, bool negative) : App(app), Ui(*app.GetImGuiSystem().Get()), Image(image), Negative(negative) {
        for (uint32_t i = 0; i < app.GetGpuSystem()->GetFlightDataCount(); ++i) Flights.push_back(make_unique<Flight>());
    }
    uint32_t Stage{0}, Frame{0}, Verified{0}, Rejected{0};
    void PrepareFrame(RenderPrepareContext& ctx) override {
        auto& flight = *Flights[ctx.App.FlightIndex];
        flight.Stage = Stage;
        flight.Frame = Frame;
        Ui.RequestOutputs(ctx.App.FlightIndex, ctx.Workloads);
    }
    void Render(RenderPipelineContext& ctx) override {
        auto& flight = *Flights[ctx.FlightIndex()];
        flight.Pending = false;
        // Deliberately drop the first few prepared snapshots. No texture acknowledgement is allowed.
        if (flight.Frame <= 4) return;
        auto graph = ctx.CreateRenderGraph("ImGui pixels and lifetime");
        const auto image = graph.CreateTexture({render::TextureDimension::Dim2D, 8, 8, 1, 1, Negative && flight.Frame == 6 ? 4u : 1u, render::TextureFormat::RGBA8_UNORM,
                                                render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource},
                                               "this frame image");
        RgTextureViewHandle view;
        if (Negative && flight.Frame == 8) {
            graph.AddComputePass<int>("uninitialized image", [&](int&, RenderGraphComputeBuilder& builder) { view = builder.ReadTexture(image); }, nullptr);
        } else
            graph.AddRasterPass<int>("producer", [&](int&, RenderGraphRasterBuilder& builder) {
            auto target = image;
            if (Negative && flight.Frame == 9) target = ctx.ImportOutputTarget(graph, App.GetWindowManager()->GetMainWindow()->GetRenderOutputId());
            view = builder.SetColorAttachment(0, target, {.Clear = {{.25f, .5f, .75f, 1}}}); }, nullptr);
        const ImGuiGraphImageBinding binding{Image, Negative && flight.Frame == 7 ? PreviousView : view};
        PreviousView = view;
        if (Negative && flight.Frame <= 10) {
            vector<ImGuiGraphImageBinding> badBindings{binding};
            if (flight.Frame == 5) badBindings.clear();
            if (flight.Frame == 10) badBindings.push_back(binding);
            ImGuiGraph::BuildGraph(graph, ctx, Ui, {}, badBindings);
            EXPECT_FALSE(graph.Compile());
            EXPECT_FALSE(graph.GetReport().Diagnostics.empty());
            ImGuiGraph::CompleteGraph(graph, ctx, Ui, false);
            ++Rejected;
            return;
        }
        EXPECT_TRUE(ImGuiGraph::BuildGraph(graph, ctx, Ui, {}, std::span{&binding, 1}));
        for (const auto& surface : ctx.OutputSurfaces()) {
            if (surface.Id != App.GetWindowManager()->GetMainWindow()->GetRenderOutputId()) continue;
            const auto output = ctx.ImportOutputTarget(graph, surface.Id);
            const auto desc = graph.GetTextureDescriptor(output);
            ASSERT_TRUE(desc);
            ASSERT_TRUE(desc->Usage.HasFlag(render::TextureUse::CopySource));
            flight.Pitch = Align(uint64_t(desc->Width) * 4, App.GetDevice()->GetDetail().TextureDataPitchAlignment);
            flight.Height = desc->Height;
            flight.Bgra = desc->Format == render::TextureFormat::BGRA8_UNORM || desc->Format == render::TextureFormat::BGRA8_UNORM_SRGB;
            const uint64_t size = flight.Pitch * flight.Height;
            if (!flight.Buffer || flight.Buffer->GetDesc().Size != size) {
                auto buffer = App.GetDevice()->CreateBuffer({size, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead});
                ASSERT_TRUE(buffer);
                flight.Buffer = buffer.Release();
            }
            flight.Import = {flight.Buffer.get(), flight.Buffer->GetDesc(), render::BufferState::CopyDestination};
            const auto host = graph.ImportBuffer(flight.Import, "UI readback", RenderGraphExternalAccess::ObservableOutput);
            graph.AddCopyTextureToBufferPass("read composed UI", output, host);
            graph.AddComputePass<int>("host visibility", [=](int&, RenderGraphComputeBuilder& builder) { builder.ReadBuffer(host, RgBufferAccess::HostRead); builder.SetSideEffect(); }, nullptr);
            flight.Pending = true;
        }
        const auto result = ctx.ExecuteGraph(graph);
        ImGuiGraph::CompleteGraph(graph, ctx, Ui, result.Success);
        EXPECT_TRUE(result.Success) << graph.GetReport().ToText();
        flight.Pending &= result.Success;
    }
    void Complete(uint32_t index) {
        auto& flight = *Flights[index];
        if (!flight.Pending) return;
        flight.Pending = false;
        ScopedBufferMap map(flight.Buffer.get(), {0, flight.Pitch * flight.Height});
        ASSERT_TRUE(map);
        const auto sample = [&](uint32_t x, uint32_t y, uint32_t c) {
            if (flight.Bgra && c != 1 && c != 3) c = 2 - c;
            return static_cast<const uint8_t*>(map.Data())[y * flight.Pitch + x * 4 + c];
        };
        const auto encode = [](float value) { return (value <= .0031308f ? 12.92f * value : 1.055f * std::pow(value, 1 / 2.4f) - .055f) * 255; };
        const float alpha = 128 / 255.0f;
        EXPECT_NEAR(sample(12, 12, 0), encode(alpha + .012f * (1 - alpha)), 2);
        EXPECT_NEAR(sample(12, 12, 1), encode(.012f * (1 - alpha)), 2);
        EXPECT_NEAR(sample(100, 12, 0), encode(64 / 255.0f), 2);
        EXPECT_NEAR(sample(100, 12, 1), encode(128 / 255.0f), 2);
        EXPECT_NEAR(sample(100, 12, 2), encode(191 / 255.0f), 2);
        EXPECT_NEAR(sample(130, 12, 1), encode(alpha + .012f * (1 - alpha)), 2);
        EXPECT_NEAR(sample(159, 12, 0), encode(.4375f), 2);
        if (flight.Stage < 2) {
            EXPECT_NEAR(sample(66, 10, 1), 255, 1);
            EXPECT_NEAR(sample(74, 18, flight.Stage == 0 ? 1 : 2), 255, 1);
            EXPECT_NEAR(sample(66, 10, 0), flight.Stage == 0 ? encode(128 / 255.0f) : 128.0f, 2);
        }
        ++Verified;
    }

private:
    Application& App;
    ImGuiSystem& Ui;
    ImTextureID Image;
    bool Negative;
    RgTextureViewHandle PreviousView;
    vector<unique_ptr<Flight>> Flights;
};
class UiProbeApp final : public Application {
public:
    explicit UiProbeApp(bool negative = false) : Negative(negative) {}
    bool SawCreateAck{false}, SawUpdateAck{false}, SawDestroyAck{false}, Clean{false};
    uint32_t Verified{0}, Rejected{0};
    bool AtlasGrew{false};

protected:
    void OnInit() override {
        Image = GetImGuiSystem()->CreateGraphImage();
        auto pipeline = make_unique<UiProbePipeline>(*this, Image, Negative);
        Pipeline = pipeline.get();
        GetRenderSystem()->SetPipeline(std::move(pipeline));
        Texture.Create(ImTextureFormat_RGBA32, 8, 8);
        for (int i = 0; i < 64; ++i) {
            Texture.Pixels[i * 4] = 128;
            Texture.Pixels[i * 4 + 1] = 255;
            Texture.Pixels[i * 4 + 3] = 255;
        }
        ImGui::RegisterUserTexture(&Texture);
        Alpha.Create(ImTextureFormat_Alpha8, 8, 8);
        std::memset(Alpha.Pixels, 128, 64);
        ImGui::RegisterUserTexture(&Alpha);
        Color.Create(ImTextureFormat_RGBA32, 2, 1);
        Color.UseColors = true;
        Color.Pixels[3] = 255;
        std::memset(Color.Pixels + 4, 255, 4);
        ImGui::RegisterUserTexture(&Color);
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (++Frame > 45) test::CloseMainWindow(*this);
        Pipeline->Frame = Frame;
        if (Frame <= (Negative ? 11u : 5u)) EXPECT_EQ(Texture.GetTexID(), ImTextureID_Invalid);
    }
    void OnImGui() override {
        if (Stage == 0 && Texture.Status == ImTextureStatus_OK && Frame > 16) {
            SawCreateAck = true;
            EXPECT_NE(Texture.GetTexID(), ImTextureID_Invalid);
            for (int y = 4; y < 8; ++y)
                for (int x = 4; x < 8; ++x) {
                    auto* pixel = static_cast<uint8_t*>(Texture.GetPixelsAt(x, y));
                    pixel[1] = 0;
                    pixel[2] = 255;
                }
            Texture.Updates.push_back({4, 4, 4, 4});
            Texture.UpdateRect = {4, 4, 4, 4};
            // Adding the first colored glyph changes the atlas sampling format, including untouched pixels.
            Texture.UseColors = true;
            Texture.SetStatus(ImTextureStatus_WantUpdates);
            Stage = 1;
        } else if (Stage == 1 && Texture.Status == ImTextureStatus_OK && Frame > 26) {
            SawUpdateAck = true;
            Texture.WantDestroyNextFrame = true;
            Stage = 2;
        } else if (Stage == 2 && Texture.Status == ImTextureStatus_Destroyed)
            SawDestroyAck = true;
        Pipeline->Stage = Stage;
        auto& atlas = *ImGui::GetIO().Fonts;
        if (Frame == 1) AtlasArea = atlas.TexData->Width * atlas.TexData->Height;
        if (Frame == 18) {
            auto* font = ImGui::GetFont()->GetFontBaked(96);
            for (ImWchar c = 32; c < 256; ++c) font->FindGlyph(c);
            AtlasGrew = atlas.TexData->Width * atlas.TexData->Height > AtlasArea;
        }
        auto* viewport = ImGui::GetMainViewport();
        auto* list = ImGui::GetForegroundDrawList(viewport);
        const auto p = viewport->Pos;
        for (uint32_t i = 0; i < 18000; ++i) list->AddRectFilled({p.x + 140, p.y + 30}, {p.x + 141, p.y + 31}, IM_COL32_WHITE);
        list->AddRectFilled({p.x + 8, p.y + 8}, {p.x + 40, p.y + 40}, IM_COL32(255, 0, 0, 128));
        list->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerNearest, nullptr);
        if (Stage < 2) list->AddImage(Texture.GetTexRef(), {p.x + 64, p.y + 8}, {p.x + 80, p.y + 24});
        list->AddImage(Image, {p.x + 96, p.y + 8}, {p.x + 112, p.y + 24});
        list->AddImage(Alpha.GetTexRef(), {p.x + 128, p.y + 8}, {p.x + 136, p.y + 16});
        list->AddCallback(ImGui::GetPlatformIO().DrawCallback_SetSamplerLinear, nullptr);
        list->AddImage(Color.GetTexRef(), {p.x + 152, p.y + 8}, {p.x + 168, p.y + 16});
        list->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, nullptr);
    }
    void OnRenderFrameComplete(const AppRenderCompleteContext& ctx) override {
        if (Pipeline && ctx.GpuWorkCompleted) Pipeline->Complete(ctx.FlightIndex);
    }
    void OnShutdown() override {
        Verified = Pipeline->Verified;
        Rejected = Pipeline->Rejected;
        Clean = !GetImGuiSystem()->HasError();
        ImGui::UnregisterUserTexture(&Texture);
        ImGui::UnregisterUserTexture(&Alpha);
        ImGui::UnregisterUserTexture(&Color);
        GetImGuiSystem()->UnregisterTexture(Image);
        Pipeline = nullptr;
        GetRenderSystem()->SetPipeline(nullptr);
    }

private:
    Nullable<UiProbePipeline*> Pipeline{nullptr};
    ImTextureID Image{0};
    uint32_t Frame{0}, Stage{0};
    ImTextureData Texture, Alpha, Color;
    bool Negative;
    int AtlasArea{0};
};
class ImGuiRenderingTest : public testing::TestWithParam<UiTestMode> {
protected:
    void SetUp() override {
        render::test::DeviceContext probe;
        const auto backend = GetParam().Backend;
        if (!render::test::TryCreateDevice(backend, probe, true)) {
            if (render::test::SetupMustFail(probe.Status, render::test::RequiredBackend(backend))) FAIL() << probe.Reason;
            GTEST_SKIP() << probe.Reason;
        }
    }
};
class DisabledUiProbeApp final : public Application {
protected:
    void OnInit() override {
        EXPECT_FALSE(GetImGuiSystem());
        EXPECT_EQ(ImGui::GetCurrentContext(), nullptr);
    }
    void OnUpdate(const AppUpdateContext&) override { test::CloseMainWindow(*this); }
    void OnImGui() override { FAIL() << "Disabled ImGui instance invoked its UI hook"; }
};
TEST_P(ImGuiRenderingTest, DisabledInstanceAndPartialInitializationRollbackLeaveNoContext) {
    ApplicationRuntimeDescriptor desc{.Backend = GetParam().Backend, .EnableValidation = true, .WindowTitle = "ImGui instance boundary", .WindowWidth = 96, .WindowHeight = 64, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO};
    DisabledUiProbeApp disabled;
    ASSERT_EQ(disabled.Run(desc), 0);
    EXPECT_EQ(ImGui::GetCurrentContext(), nullptr);
    desc.ImGui.Enabled = true;
    desc.ImGui.Fonts.push_back({"__radray_missing_imgui_font__.ttf"});
    DisabledUiProbeApp failed;
    test::RuntimeLogCapture logs;
    EXPECT_EQ(failed.Run(desc), 1);
    EXPECT_FALSE(failed.GetImGuiSystem());
    EXPECT_EQ(ImGui::GetCurrentContext(), nullptr);
    EXPECT_NE(logs.Errors().find("ImGui font read failed"), string::npos);
}
class UiWindowProbeApp final : public Application {
public:
    bool SawAuxiliary{false}, SawClosed{false}, SawReopened{false}, Clean{false};
    uint32_t ToolInput{0};

protected:
    void OnInit() override { GetRenderSystem()->SetPipeline(make_unique<ImGuiOnlyPipeline>(*GetImGuiSystem().Get())); }
    void OnUpdate(const AppUpdateContext&) override {
        ++Frame;
        auto* manager = GetWindowManager();
        auto* main = manager->GetMainWindow()->GetNativeWindow();
        if (Frame >= 4 && Frame <= 8) main->SetSize(240 + int(Frame) * 4, 160 + int(Frame) * 2);
        for (size_t i = 0; i < manager->GetWindowCount(); ++i) {
            auto* window = manager->GetWindow(i);
            if (window->GetOutputUsage() != RenderOutputUsage::Auxiliary) continue;
            SawAuxiliary = true;
            if (Frame > 18) SawReopened = true;
            auto* native = window->GetNativeWindow();
            if (Frame == 5) {
                Input = window->GetInput().EventInput().connect([this](const WindowInputEvent&) { ++ToolInput; });
                native->EventKeyboard()(KeyCode::B, Action::PRESSED);
                native->EventScroll()(.25f, -.5f);
            }
#ifdef _WIN32
            const auto hwnd = static_cast<HWND>(native->GetNativeHandler());
            if (Frame == 6) ::ShowWindow(hwnd, SW_MINIMIZE);
            if (Frame == 8) ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            if (Frame == 11) ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
#endif
        }
        if (Frame == 17) {
            SawClosed = manager->GetWindowCount() == 1;
            EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_B));
        }
        if (Frame == 18) Tool = true;
        if (Frame > 30) test::CloseMainWindow(*this);
    }
    void OnImGui() override {
        // Native modal callbacks can occur while frame construction is on the stack.
        GetWindowManager()->EventModalLoopTick()(GetWindowManager()->GetMainWindow()->GetNativeWindow());
        if (!Tool) return;
        ImGui::SetNextWindowPos({-40, 40}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({float(210 + (Frame % 4) * 7), 130}, ImGuiCond_Always);
        ImGui::Begin("auxiliary lifecycle", &Tool);
        ImGui::TextUnformatted("resize / minimize / close / recreate");
        ImGui::End();
    }
    void OnShutdown() override {
        Clean = !GetImGuiSystem()->HasError();
        Input.disconnect();
        GetRenderSystem()->SetPipeline(nullptr);
    }

private:
    bool Tool{true};
    uint32_t Frame{0};
    sigslot::scoped_connection Input;
};
TEST_P(ImGuiRenderingTest, AuxiliaryWindowsResizeMinimizeCloseRecreateAndRejectModalFrameReentry) {
    const auto mode = GetParam();
#ifndef RADRAY_ENABLE_D3D12
    if (mode.Backend == render::RenderBackend::D3D12) GTEST_SKIP() << "D3D12 backend not built";
#endif
#ifndef RADRAY_ENABLE_VULKAN
    if (mode.Backend == render::RenderBackend::Vulkan) GTEST_SKIP() << "Vulkan backend not built";
#endif
    test::RuntimeLogCapture logs;
    UiWindowProbeApp app;
    ApplicationRuntimeDescriptor desc{.Backend = mode.Backend, .EnableValidation = true, .Multithreaded = mode.Threaded, .WindowTitle = "ImGui viewport lifecycle regression", .WindowWidth = 240, .WindowHeight = 160, .FlightDataCount = mode.Flights, .BackBufferFormat = mode.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO, .EnableSynchronizationValidation = true};
    desc.ImGui.Enabled = true;
    ASSERT_EQ(app.Run(desc), 0);
    EXPECT_TRUE(app.SawAuxiliary);
    EXPECT_TRUE(app.SawClosed);
    EXPECT_TRUE(app.SawReopened);
    EXPECT_TRUE(app.Clean);
    EXPECT_EQ(app.ToolInput, 0u);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
TEST_P(ImGuiRenderingTest, DynamicTextureRegionsOffsetsGraphImagesAndLinearBlendSurviveDroppedFrames) {
    const auto mode = GetParam();
#ifndef RADRAY_ENABLE_D3D12
    if (mode.Backend == render::RenderBackend::D3D12) GTEST_SKIP() << "D3D12 backend not built";
#endif
#ifndef RADRAY_ENABLE_VULKAN
    if (mode.Backend == render::RenderBackend::Vulkan) GTEST_SKIP() << "Vulkan backend not built";
#endif
    test::RuntimeLogCapture logs;
    UiProbeApp app;
    ApplicationRuntimeDescriptor desc{.Backend = mode.Backend, .EnableValidation = true, .Multithreaded = mode.Threaded, .WindowTitle = "ImGui pixel/lifetime regression", .WindowWidth = 240, .WindowHeight = 160, .FlightDataCount = mode.Flights, .BackBufferFormat = mode.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO, .EnableSynchronizationValidation = true};
    desc.ImGui.Enabled = true;
    ASSERT_EQ(app.Run(desc), 0);
    EXPECT_TRUE(app.Clean);
    EXPECT_TRUE(app.SawCreateAck);
    EXPECT_TRUE(app.SawUpdateAck);
    EXPECT_TRUE(app.SawDestroyAck);
    EXPECT_GT(app.Verified, 20u);
    EXPECT_TRUE(app.AtlasGrew);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
TEST_P(ImGuiRenderingTest, RejectsMissingStaleMsaaUninitializedFeedbackAndDuplicateGraphImagesWithoutAcknowledgingUploads) {
    const auto mode = GetParam();
#ifndef RADRAY_ENABLE_D3D12
    if (mode.Backend == render::RenderBackend::D3D12) GTEST_SKIP() << "D3D12 backend not built";
#endif
#ifndef RADRAY_ENABLE_VULKAN
    if (mode.Backend == render::RenderBackend::Vulkan) GTEST_SKIP() << "Vulkan backend not built";
#endif
    test::RuntimeLogCapture logs;
    UiProbeApp app(true);
    ApplicationRuntimeDescriptor desc{.Backend = mode.Backend, .EnableValidation = true, .Multithreaded = mode.Threaded, .WindowTitle = "ImGui rejected graph regression", .WindowWidth = 240, .WindowHeight = 160, .FlightDataCount = mode.Flights, .BackBufferFormat = mode.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO, .EnableSynchronizationValidation = true};
    desc.ImGui.Enabled = true;
    ASSERT_EQ(app.Run(desc), 0);
    EXPECT_FALSE(app.Clean);
    EXPECT_EQ(app.Rejected, 6u);
    EXPECT_TRUE(app.SawCreateAck);
    EXPECT_TRUE(app.SawUpdateAck);
    EXPECT_TRUE(app.SawDestroyAck);
    EXPECT_TRUE(app.AtlasGrew);
    EXPECT_GT(app.Verified, 20u);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
INSTANTIATE_TEST_SUITE_P(Backends, ImGuiRenderingTest, testing::Values(UiTestMode{render::RenderBackend::D3D12, false, false, 2}, UiTestMode{render::RenderBackend::D3D12, true, true, 3}, UiTestMode{render::RenderBackend::Vulkan, false, false, 2}, UiTestMode{render::RenderBackend::Vulkan, true, true, 3}));
}  // namespace
}  // namespace radray
