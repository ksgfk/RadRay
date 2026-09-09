#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include <cstring>
#include <gtest/gtest.h>
#include <imgui_internal.h>
#include <radray/imgui/imgui_graph.h>
#include <radray/runtime/render_framework/render_graph_blit.h>
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
        RgReadbackTicket Readback;
        uint64_t Pitch{0};
        uint32_t Height{0}, Stage{0}, Frame{0};
        bool Pending{false}, Bgra{false};
    };
    UiProbePipeline(Application& app, ImGuiSystem& ui, ImTextureID image, bool negative, bool outputPreview) : App(app), Ui(ui), Image(image), Negative(negative), OutputPreview(outputPreview) {
        for (uint32_t i = 0; i < app.GetGpuSystem()->GetFlightDataCount(); ++i) Flights.push_back(make_unique<Flight>());
    }
    uint32_t Stage{0}, Frame{0}, Verified{0}, Rejected{0};
    void PrepareFrame(RenderPrepareContext& ctx) override {
        auto& flight = *Flights[ctx.App.FlightIndex];
        flight.Stage = Stage;
        flight.Frame = Frame;
        Ui.RequestOutputs(ctx.App.FlightIndex, ctx.Workloads);
    }
    void BuildGraph(RenderPipelineContext& ctx, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
        auto& flight = *Flights[ctx.FlightIndex()];
        flight.Pending = false;
        // Deliberately drop the first few prepared snapshots. No texture acknowledgement is allowed.
        if (flight.Frame <= 4) return;
        auto image = graph.CreateTexture({render::TextureDimension::Dim2D, 8, 8, 1, 1, Negative && flight.Frame == 6 ? 4u : 1u, render::TextureFormat::RGBA8_UNORM,
                                          render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource},
                                         "this frame image");
        if (!(Negative && flight.Frame == 8))
            graph.AddRasterPass<int>("producer", [&](int&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, image, {.Clear = {{.25f, .5f, .75f, 1}}}); }, nullptr);
        vector<RenderGraphOutputBinding> uiTargets;
        for (const auto& output : outputs) {
            auto descriptor = graph.GetTextureDescriptor(output.Texture).value();
            descriptor.Format = render::TextureFormat::RGBA16_FLOAT;
            descriptor.Hints = render::ResourceHint::None;
            descriptor.Usage = render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource;
            auto canvas = graph.CreateTexture(descriptor, "test linear display");
            if (OutputPreview && !(Negative && flight.Frame <= 10))
                canvas = AddRenderGraphBlit(graph, *App.GetRenderSystem(), App.GetDevice()->GetBackend(), image, canvas, true);
            else
                graph.AddRasterPass<int>("display clear", [=](int&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, canvas, {.Clear = {{.012f, .012f, .012f, 1}}}); }, nullptr);
            uiTargets.push_back({output.Output, canvas});
        }
        if (Negative && flight.Frame == 9) image = uiTargets.front().Texture;
        const auto previousImage = PreviousImage;
        const ImGuiGraphImageBinding binding{Image, Negative && flight.Frame == 7 ? previousImage : image};
        PreviousImage = image;
        if (Negative && flight.Frame <= 10) {
            vector<ImGuiGraphImageBinding> badBindings{binding};
            if (flight.Frame == 5) badBindings.clear();
            if (flight.Frame == 10) badBindings.push_back(binding);
            if (OutputPreview) {
                vector<ImGuiSceneOutput> scenes{{App.GetWindowManager()->GetMainWindow()->GetRenderOutputId(), image, ImGuiColorEncoding::Srgb}};
                if (flight.Frame == 5) scenes.clear();
                if (flight.Frame == 7) scenes.front().Texture = previousImage;
                if (flight.Frame == 9) scenes.front().Texture = uiTargets.front().Texture;
                if (flight.Frame == 10) scenes.push_back(scenes.front());
                ImGuiGraph::BuildGraph(graph, ctx, Ui.GetGraphFrame(ctx.FlightIndex()), uiTargets, scenes);
            } else
                ImGuiGraph::BuildGraph(graph, ctx, Ui.GetGraphFrame(ctx.FlightIndex()), uiTargets, {}, badBindings);
            return;
        }
        const ImGuiSceneOutput scene{App.GetWindowManager()->GetMainWindow()->GetRenderOutputId(), image, ImGuiColorEncoding::Srgb};
        EXPECT_TRUE(OutputPreview ? ImGuiGraph::BuildGraph(graph, ctx, Ui.GetGraphFrame(ctx.FlightIndex()), uiTargets, std::span{&scene, 1})
                                  : ImGuiGraph::BuildGraph(graph, ctx, Ui.GetGraphFrame(ctx.FlightIndex()), uiTargets, {}, std::span{&binding, 1}));
        for (auto& output : outputs) {
            auto source = FindGraphOutput(uiTargets, output.Output);
            const auto format = graph.GetTextureDescriptor(output.Texture)->Format;
            const bool srgb = format == render::TextureFormat::RGBA8_UNORM_SRGB || format == render::TextureFormat::BGRA8_UNORM_SRGB;
            output.Texture = AddRenderGraphBlit(graph, *App.GetRenderSystem(), App.GetDevice()->GetBackend(), source->Texture, output.Texture, false, !srgb);
        }
        for (const auto& surface : ctx.OutputSurfaces()) {
            if (surface.Id != App.GetWindowManager()->GetMainWindow()->GetRenderOutputId()) continue;
            const auto output = FindGraphOutput(outputs, surface.Id)->Texture;
            const auto desc = graph.GetTextureDescriptor(output);
            ASSERT_TRUE(desc);
            ASSERT_TRUE(desc->Usage.HasFlag(render::TextureUse::CopySource));
            flight.Pitch = Align(uint64_t(desc->Width) * 4, App.GetDevice()->GetDetail().TextureDataPitchAlignment);
            flight.Height = desc->Height;
            flight.Bgra = desc->Format == render::TextureFormat::BGRA8_UNORM || desc->Format == render::TextureFormat::BGRA8_UNORM_SRGB;
            flight.Readback = graph.ReadbackTexture("read composed UI", output);
            flight.Pending = true;
        }
    }
    void GraphRecorded(RenderPipelineContext& ctx, const RenderGraph& graph, RenderGraphExecutionResult result) override {
        auto& flight = *Flights[ctx.FlightIndex()];
        if (flight.Frame <= 4) return;
        if (Negative && flight.Frame <= 10) {
            EXPECT_FALSE(result.Success);
            EXPECT_FALSE(graph.GetReport().Diagnostics.empty());
            ImGuiGraph::CompleteGraph(graph, ctx, Ui.GetGraphFrame(ctx.FlightIndex()), false);
            ++Rejected;
            return;
        }
        ImGuiGraph::CompleteGraph(graph, ctx, Ui.GetGraphFrame(ctx.FlightIndex()), result.Success);
        EXPECT_TRUE(result.Success) << graph.GetReport().ToText();
        flight.Pending &= result.Success;
    }
    void Complete(uint32_t index) {
        auto& flight = *Flights[index];
        if (!flight.Pending) return;
        flight.Pending = false;
        vector<byte> bytes;
        ASSERT_TRUE(flight.Readback.Read(bytes));
        const auto sample = [&](uint32_t x, uint32_t y, uint32_t c) {
            if (flight.Bgra && c != 1 && c != 3) c = 2 - c;
            return reinterpret_cast<const uint8_t*>(bytes.data())[y * flight.Pitch + x * 4 + c];
        };
        const auto encode = [](float value) { return (value <= .0031308f ? 12.92f * value : 1.055f * std::pow(value, 1 / 2.4f) - .055f) * 255; };
        const auto decode = [](float value) { return value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f); };
        const float backgroundR = OutputPreview ? decode(64 / 255.f) : .012f;
        const float backgroundG = OutputPreview ? decode(128 / 255.f) : .012f;
        const float alpha = 128 / 255.0f;
        EXPECT_NEAR(sample(12, 12, 0), encode(alpha + backgroundR * (1 - alpha)), 2);
        EXPECT_NEAR(sample(12, 12, 1), encode(backgroundG * (1 - alpha)), 2);
        EXPECT_NEAR(sample(100, 12, 0), OutputPreview ? 64.f : encode(64 / 255.0f), 2);
        EXPECT_NEAR(sample(100, 12, 1), OutputPreview ? 128.f : encode(128 / 255.0f), 2);
        EXPECT_NEAR(sample(100, 12, 2), OutputPreview ? 191.f : encode(191 / 255.0f), 2);
        EXPECT_NEAR(sample(130, 12, 1), encode(alpha + backgroundG * (1 - alpha)), 2);
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
    bool OutputPreview;
    RgTextureValue PreviousImage;
    vector<unique_ptr<Flight>> Flights;
};
class UiProbeApp final : public Application {
public:
    explicit UiProbeApp(bool negative = false, bool outputPreview = false) : Negative(negative), OutputPreview(outputPreview) {}
    bool SawCreateAck{false}, SawUpdateAck{false}, SawDestroyAck{false}, Clean{false};
    uint32_t Verified{0}, Rejected{0};
    bool AtlasGrew{false};

protected:
    void OnInit() override {
        ImGuiSystemDescriptor descriptor;
        descriptor.InstallDefaultOverlay = false;
        Ui = ImGuiSystem::Install(*this, descriptor);
        ASSERT_TRUE(Ui);
        UiDraw = Ui->EventDraw().connect(&UiProbeApp::DrawUi, this);
        Image = OutputPreview ? Ui->RegisterOutput(GetWindowManager()->GetMainWindow()->GetRenderOutputId()) : Ui->CreateGraphImage();
        auto pipeline = make_unique<UiProbePipeline>(*this, *Ui.Get(), Image, Negative, OutputPreview);
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
    void DrawUi() {
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
    void OnRenderFrameComplete(const FlightCompletion& ctx) override {
        if (Pipeline && ctx.GpuWorkCompleted) Pipeline->Complete(ctx.FlightIndex);
    }
    void OnShutdown() override {
        Verified = Pipeline->Verified;
        Rejected = Pipeline->Rejected;
        UiDraw.disconnect();
        Clean = !Ui->HasError();
        ImGui::UnregisterUserTexture(&Texture);
        ImGui::UnregisterUserTexture(&Alpha);
        ImGui::UnregisterUserTexture(&Color);
        Ui->UnregisterTexture(Image);
        Ui = nullptr;
        Pipeline = nullptr;
        GetRenderSystem()->SetPipeline(nullptr);
    }

private:
    Nullable<UiProbePipeline*> Pipeline{nullptr};
    Nullable<ImGuiSystem*> Ui{nullptr};
    sigslot::scoped_connection UiDraw;
    ImTextureID Image{0};
    uint32_t Frame{0}, Stage{0};
    ImTextureData Texture, Alpha, Color;
    bool Negative;
    bool OutputPreview;
    int AtlasArea{0};
};

class UiTextureComposer final : public FrameGraphComposer {
public:
    struct Consumer final : RenderGraphComponent {
        RenderSystem& Renderer;
        render::RenderBackend Backend;
        uint32_t Pass{RgInvalidIndex};
        explicit Consumer(RenderSystem& renderer, render::RenderBackend backend) : Renderer(renderer), Backend(backend) {}
        void BuildGraph(RenderPipelineContext&, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
            Pass = RgInvalidIndex;
            for (auto& output : outputs) {
                const auto desc = graph.GetTextureDescriptor(output.Texture);
                ASSERT_TRUE(desc);
                const auto destination = graph.CreateTexture(*desc, "Scene.MaterialTarget");
                Pass = static_cast<uint32_t>(graph.GetReport().Passes.size());
                output.Texture = AddRenderGraphBlit(graph, Renderer, Backend, output.Texture, destination);
            }
        }
        void GraphRecorded(RenderPipelineContext&, const RenderGraph& graph, RenderGraphExecutionResult result) override {
            ASSERT_TRUE(result.Success) << graph.GetReport().ToText();
            if (Pass == RgInvalidIndex) return;
            const auto& report = graph.GetReport();
            uint32_t ui = RgInvalidIndex;
            for (uint32_t p = 0; p < report.Passes.size(); ++p)
                if (report.Passes[p].Name == "ImGui.Draw") ui = p;
            ASSERT_NE(ui, RgInvalidIndex) << report.ToText();
            EXPECT_LT(Pass, ui);  // Consumer setup intentionally precedes the UI producer.
            EXPECT_LT(std::find(report.ExecutionOrder.begin(), report.ExecutionOrder.end(), ui), std::find(report.ExecutionOrder.begin(), report.ExecutionOrder.end(), Pass));
            EXPECT_NE(std::find(report.Passes[Pass].DataDependencies.begin(), report.Passes[Pass].DataDependencies.end(), ui), report.Passes[Pass].DataDependencies.end());
        }
    };
    struct Readback {
        RgReadbackTicket Ticket;
        uint64_t Pitch{0};
        bool Bgra{false};
    };
    UiTextureComposer(Application& app, ImGuiSystem& ui) : Ui(ui), Renderer(*app.GetRenderSystem()), Backend(app.GetDevice()->GetBackend()), UiComponent(ui.GetGraphComponent()), SceneConsumer(Renderer, Backend), Readbacks(app.GetGpuSystem()->GetFlightDataCount()) {}
    void PrepareFrame(RenderPrepareContext& context) override { UiComponent.PrepareFrame(context); }
    void Compose(FrameGraph& frame, Nullable<RenderPipeline*>) override {
        vector<FrameGraphOutputDesc> descriptions;
        for (const auto& surface : frame.Context.OutputSurfaces()) {
            auto desc = surface.Desc;
            desc.Format = render::TextureFormat::RGBA16_FLOAT;
            desc.Hints = render::ResourceHint::None;
            desc.Usage = render::TextureUse::Resource | render::TextureUse::RenderTarget | render::TextureUse::CopySource;
            descriptions.push_back({surface.Id, desc});
        }
        const auto scene = frame.AddComponent("Scene consuming UI texture", SceneConsumer, descriptions);
        const auto ui = frame.AddComponent("UI producing texture", UiComponent, descriptions);
        auto& readback = Readbacks[frame.Context.FlightIndex()];
        readback = {};
        for (const auto& desc : descriptions) {
            const auto canvas = frame.Graph.CreateTexture(desc.Desc, "UI.Offscreen");
            frame.Graph.AddRasterPass<int>("UI background", [=](int&, RenderGraphRasterBuilder& b) { b.SetColorAttachment(0, canvas, {.Clear = {{0, 0, 0, 1}}}); }, nullptr);
            frame.Graph.Connect(frame.Input(ui, desc.Output), canvas);
            frame.Graph.Connect(frame.Input(scene, desc.Output), frame.Graph.Value(frame.Output(ui, desc.Output)));
            const auto destination = frame.Context.ImportOutputTarget(frame.Graph, desc.Output);
            const auto format = frame.Graph.GetTextureDescriptor(destination)->Format;
            const bool srgb = format == render::TextureFormat::BGRA8_UNORM_SRGB || format == render::TextureFormat::RGBA8_UNORM_SRGB;
            const auto finalValue = AddRenderGraphBlit(frame.Graph, Renderer, Backend, frame.Graph.Value(frame.Output(scene, desc.Output)), destination, false, !srgb);
            for (const auto& surface : frame.Context.OutputSurfaces())
                if (surface.Id == desc.Output) frame.Graph.ExportTexture(finalValue, surface.RequiredFinalState);
            readback.Ticket = frame.Graph.ReadbackTexture("scene sampled UI", finalValue);
            readback.Pitch = Align(uint64_t(desc.Desc.Width) * 4, frame.Context.Capabilities().Detail.TextureDataPitchAlignment);
            readback.Bgra = format == render::TextureFormat::BGRA8_UNORM || format == render::TextureFormat::BGRA8_UNORM_SRGB;
        }
    }
    void Complete(uint32_t flight) {
        auto& readback = Readbacks[flight];
        if (!readback.Ticket.IsValid()) return;
        vector<byte> pixels;
        ASSERT_TRUE(readback.Ticket.Read(pixels));
        const auto offset = 16 * readback.Pitch + 16 * 4;
        ASSERT_LT(offset + 3, pixels.size());
        uint64_t redCount = 0, firstRed = UINT64_MAX;
        for (size_t i = 0; i + 3 < pixels.size(); i += 4)
            if (uint8_t(pixels[i + (readback.Bgra ? 2 : 0)]) > 200) {
                ++redCount;
                firstRed = std::min<uint64_t>(firstRed, i);
            }
        EXPECT_EQ(uint8_t(pixels[offset + (readback.Bgra ? 2 : 0)]), 255) << "pitch=" << readback.Pitch << " red=" << redCount << " first=" << firstRed;
        EXPECT_EQ(uint8_t(pixels[offset + 1]), 0);
        EXPECT_EQ(uint8_t(pixels[offset + (readback.Bgra ? 0 : 2)]), 0);
        ++Verified;
        readback = {};
    }
    ImGuiSystem& Ui;
    RenderSystem& Renderer;
    render::RenderBackend Backend;
    ImGuiGraphComponent& UiComponent;
    Consumer SceneConsumer;
    vector<Readback> Readbacks;
    uint32_t Verified{0};
};
class UiTextureApp final : public Application {
public:
    uint32_t Verified{0};
    void OnInit() override {
        ImGuiSystemDescriptor descriptor;
        descriptor.InstallDefaultOverlay = false;
        Ui = ImGuiSystem::Install(*this, descriptor);
        ASSERT_TRUE(Ui);
        UiDraw = Ui->EventDraw().connect(&UiTextureApp::DrawUi, this);
        auto composer = make_unique<UiTextureComposer>(*this, *Ui.Get());
        Composer = composer.get();
        ASSERT_TRUE(GetRenderSystem()->SetGraphComposer(std::move(composer)));
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (++Frames > 20) test::CloseMainWindow(*this);
    }
    void DrawUi() {
        auto* viewport = ImGui::GetMainViewport();
        const auto p = viewport->Pos;
        ImGui::GetForegroundDrawList(viewport)->AddRectFilled({p.x + 8, p.y + 8}, {p.x + 40, p.y + 40}, IM_COL32(255, 0, 0, 255));
    }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        if (Composer && completion.GpuWorkCompleted) Composer->Complete(completion.FlightIndex);
    }
    void OnShutdown() override {
        Verified = Composer->Verified;
        UiDraw.disconnect();
        EXPECT_FALSE(Ui->HasError());
        Ui = nullptr;
        Composer = nullptr;
        ASSERT_TRUE(GetRenderSystem()->SetGraphComposer(nullptr));
    }

private:
    Nullable<UiTextureComposer*> Composer{nullptr};
    Nullable<ImGuiSystem*> Ui{nullptr};
    sigslot::scoped_connection UiDraw;
    uint32_t Frames{0};
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
public:
    explicit DisabledUiProbeApp(bool attemptFailingInstall = false) : AttemptFailingInstall(attemptFailingInstall) {}
    bool InstallReturnedNull{false};
    size_t OverlayCount{SIZE_MAX};

protected:
    void OnInit() override {
        EXPECT_EQ(ImGui::GetCurrentContext(), nullptr);
        if (!AttemptFailingInstall) return;
        ImGuiSystemDescriptor descriptor;
        descriptor.Fonts.push_back({"__radray_missing_imgui_font__.ttf"});
        InstallReturnedNull = !ImGuiSystem::Install(*this, descriptor);
        OverlayCount = GetRenderSystem()->GetOverlays().size();
        EXPECT_EQ(ImGui::GetCurrentContext(), nullptr);
    }
    void OnUpdate(const AppUpdateContext&) override { test::CloseMainWindow(*this); }

private:
    bool AttemptFailingInstall;
};
TEST_P(ImGuiRenderingTest, DisabledInstanceAndPartialInitializationRollbackLeaveNoContext) {
    ApplicationRuntimeDescriptor desc{.Backend = GetParam().Backend, .EnableValidation = true, .WindowTitle = "ImGui instance boundary", .WindowWidth = 96, .WindowHeight = 64, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO};
    DisabledUiProbeApp disabled;
    ASSERT_EQ(disabled.Run(desc), 0);
    EXPECT_EQ(ImGui::GetCurrentContext(), nullptr);
    DisabledUiProbeApp failed(true);
    test::RuntimeLogCapture logs;
    EXPECT_EQ(failed.Run(desc), 0);
    EXPECT_TRUE(failed.InstallReturnedNull);
    EXPECT_EQ(failed.OverlayCount, 0u);
    EXPECT_EQ(ImGui::GetCurrentContext(), nullptr);
    EXPECT_NE(logs.Errors().find("ImGui font read failed"), string::npos);
}
class UiWindowProbeApp final : public Application {
public:
    bool SawAuxiliary{false}, SawClosed{false}, SawReopened{false}, Clean{false};
    uint32_t ToolInput{0};

protected:
    void OnInit() override {
        Ui = ImGuiSystem::Install(*this, {});
        ASSERT_TRUE(Ui);
        UiDraw = Ui->EventDraw().connect(&UiWindowProbeApp::DrawUi, this);
    }
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
            if (Frame == 6) {
                GetWindowManager()->EnsureRenderIdle();
                ::ShowWindow(hwnd, SW_MINIMIZE);
            }
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
    void DrawUi() {
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
        UiDraw.disconnect();
        Clean = !Ui->HasError();
        Ui = nullptr;
        Input.disconnect();
        GetRenderSystem()->SetPipeline(nullptr);
    }

private:
    bool Tool{true};
    uint32_t Frame{0};
    Nullable<ImGuiSystem*> Ui{nullptr};
    sigslot::scoped_connection UiDraw;
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
    ApplicationRuntimeDescriptor desc{.Backend = mode.Backend, .EnableValidation = true, .Multithreaded = mode.Threaded, .EnableSynchronizationValidation = true, .WindowTitle = "ImGui viewport lifecycle regression", .WindowWidth = 240, .WindowHeight = 160, .FlightDataCount = mode.Flights, .BackBufferFormat = mode.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO};
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
    ApplicationRuntimeDescriptor desc{.Backend = mode.Backend, .EnableValidation = true, .Multithreaded = mode.Threaded, .EnableSynchronizationValidation = true, .WindowTitle = "ImGui pixel/lifetime regression", .WindowWidth = 240, .WindowHeight = 160, .FlightDataCount = mode.Flights, .BackBufferFormat = mode.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO};
    ASSERT_EQ(app.Run(desc), 0);
    EXPECT_TRUE(app.Clean);
    EXPECT_TRUE(app.SawCreateAck);
    EXPECT_TRUE(app.SawUpdateAck);
    EXPECT_TRUE(app.SawDestroyAck);
    EXPECT_GT(app.Verified, 20u);
    EXPECT_TRUE(app.AtlasGrew);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
TEST_P(ImGuiRenderingTest, OutputPreviewSamplesSceneBeforeUiAndDecodesSrgbOnce) {
    const auto mode = GetParam();
    test::RuntimeLogCapture logs;
    UiProbeApp app(false, true);
    ApplicationRuntimeDescriptor desc{.Backend = mode.Backend, .EnableValidation = true, .Multithreaded = mode.Threaded, .EnableSynchronizationValidation = true, .WindowTitle = "ImGui output preview", .WindowWidth = 240, .WindowHeight = 160, .FlightDataCount = mode.Flights, .BackBufferFormat = mode.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO};
    ASSERT_EQ(app.Run(desc), 0);
    EXPECT_TRUE(app.Clean);
    EXPECT_GT(app.Verified, 20u);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
TEST_P(ImGuiRenderingTest, OutputPreviewRejectsMissingStaleMsaaUninitializedFeedbackAndDuplicateScenes) {
    const auto mode = GetParam();
    test::RuntimeLogCapture logs;
    logs.ExpectedGraphErrors = 6;
    UiProbeApp app(true, true);
    ApplicationRuntimeDescriptor desc{.Backend = mode.Backend, .EnableValidation = true, .Multithreaded = mode.Threaded, .EnableSynchronizationValidation = true, .WindowTitle = "ImGui invalid output preview", .WindowWidth = 240, .WindowHeight = 160, .FlightDataCount = mode.Flights, .BackBufferFormat = mode.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO};
    ASSERT_EQ(app.Run(desc), 0);
    EXPECT_FALSE(app.Clean);
    EXPECT_EQ(app.Rejected, 6u);
    EXPECT_EQ(logs.ExpectedGraphErrors.load(), 0u);
    EXPECT_GT(app.Verified, 20u);
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
    logs.ExpectedGraphErrors = 6;
    UiProbeApp app(true);
    ApplicationRuntimeDescriptor desc{.Backend = mode.Backend, .EnableValidation = true, .Multithreaded = mode.Threaded, .EnableSynchronizationValidation = true, .WindowTitle = "ImGui rejected graph regression", .WindowWidth = 240, .WindowHeight = 160, .FlightDataCount = mode.Flights, .BackBufferFormat = mode.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO};
    ASSERT_EQ(app.Run(desc), 0);
    EXPECT_FALSE(app.Clean);
    EXPECT_EQ(app.Rejected, 6u);
    EXPECT_EQ(logs.ExpectedGraphErrors.load(), 0u);
    EXPECT_TRUE(app.SawCreateAck);
    EXPECT_TRUE(app.SawUpdateAck);
    EXPECT_TRUE(app.SawDestroyAck);
    EXPECT_TRUE(app.AtlasGrew);
    EXPECT_GT(app.Verified, 20u);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST_P(ImGuiRenderingTest, UiTextureFeedsSceneComponentDeclaredBeforeItsProducer) {
    const auto mode = GetParam();
    test::RuntimeLogCapture logs;
    UiTextureApp app;
    ApplicationRuntimeDescriptor desc{.Backend = mode.Backend, .EnableValidation = true, .Multithreaded = mode.Threaded, .EnableSynchronizationValidation = true, .WindowTitle = "UI texture consumed by scene", .WindowWidth = 96, .WindowHeight = 64, .FlightDataCount = mode.Flights, .BackBufferFormat = mode.Srgb ? render::TextureFormat::BGRA8_UNORM_SRGB : render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO};
    ASSERT_EQ(app.Run(desc), 0);
    EXPECT_GE(app.Verified, 15u);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
INSTANTIATE_TEST_SUITE_P(Backends, ImGuiRenderingTest, testing::Values(UiTestMode{render::RenderBackend::D3D12, false, false, 2}, UiTestMode{render::RenderBackend::D3D12, true, true, 3}, UiTestMode{render::RenderBackend::Vulkan, false, false, 2}, UiTestMode{render::RenderBackend::Vulkan, true, true, 3}));
}  // namespace
}  // namespace radray
