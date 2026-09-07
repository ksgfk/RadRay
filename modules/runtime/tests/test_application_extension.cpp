#include "runtime_test_support.h"
#include "gpu_test_fixture.h"

#include <algorithm>

#include <gtest/gtest.h>

#include <radray/runtime/application_extension.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {

struct ExtensionTrace {
    vector<string> Events;
    bool AddAfterLoopRejected{false};
};

class TraceExtension final : public ApplicationExtension {
public:
    explicit TraceExtension(ExtensionTrace& trace, string name) : _trace(trace), _name(std::move(name)) {}
    void OnBeginUpdate(uint32_t) override { _trace.Events.push_back(_name + ".BeginUpdate"); }
    void OnBeforeInput(const AppUpdateContext&) override { _trace.Events.push_back(_name + ".BeforeInput"); }
    void OnAfterWorldTick(const AppUpdateContext&) override { _trace.Events.push_back(_name + ".AfterWorldTick"); }

private:
    ExtensionTrace& _trace;
    string _name;
};

class TracePipeline final : public RenderPipeline {
public:
    explicit TracePipeline(ExtensionTrace& trace) : _trace(trace) {}
    void PrepareFrame(RenderPrepareContext& prepare) override {
        prepare.Workloads.AddPresentationOutputs();
        _trace.Events.push_back("Pipeline.PrepareFrame");
    }
    void BuildGraph(RenderPipelineContext&, RenderGraph&, std::span<RenderGraphOutputBinding>) override {}

private:
    ExtensionTrace& _trace;
};

class ExtensionOrderApp final : public Application {
public:
    explicit ExtensionOrderApp(ExtensionTrace& trace) : _trace(trace) {}

protected:
    void OnInit() override {
        ASSERT_TRUE(AddExtension(make_unique<TraceExtension>(_trace, "A")));
        ASSERT_TRUE(AddExtension(make_unique<TraceExtension>(_trace, "B")));
        EXPECT_FALSE(AddExtension(nullptr));
        GetRenderSystem()->SetPipeline(make_unique<TracePipeline>(_trace));
    }
    void OnUpdate(const AppUpdateContext&) override {
        _trace.Events.push_back("OnUpdate");
        if (_updates == 0) {
            _trace.AddAfterLoopRejected = !AddExtension(make_unique<TraceExtension>(_trace, "Late"));
        }
        if (++_updates >= 2) test::CloseMainWindow(*this);
    }

private:
    ExtensionTrace& _trace;
    uint32_t _updates{0};
};

TEST(ApplicationExtension, SlotsRunInInstallationOrderAroundInputWorldTickAndPrepareFrame) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, device)) GTEST_SKIP() << device.Reason;
    }
    ExtensionTrace trace;
    test::RuntimeLogCapture logs;
    ExtensionOrderApp app{trace};
    ASSERT_EQ(app.Run({.Backend = render::RenderBackend::D3D12, .EnableValidation = true, .WindowTitle = "extension order", .WindowWidth = 160, .WindowHeight = 120, .FlightDataCount = 2, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_TRUE(trace.AddAfterLoopRejected);
    const vector<string> frame{"A.BeginUpdate", "B.BeginUpdate", "A.BeforeInput", "B.BeforeInput", "OnUpdate", "A.AfterWorldTick", "B.AfterWorldTick", "Pipeline.PrepareFrame"};
    ASSERT_GE(trace.Events.size(), frame.size());
    for (size_t i = 0; i < frame.size(); ++i) EXPECT_EQ(trace.Events[i], frame[i]) << i;
    // The rejected late extension logs exactly one error.
    EXPECT_NE(logs.Errors().find("before the main loop starts"), string::npos);
}

class SolidOverlay final : public RenderGraphComponent {
public:
    void BuildGraph(RenderPipelineContext&, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
        for (auto& output : outputs) {
            const auto desc = graph.GetTextureDescriptor(output.Texture);
            ASSERT_TRUE(desc);
            EXPECT_EQ(desc->Format, render::TextureFormat::RGBA16_FLOAT);
            auto target = graph.NextVersion(output.Texture);
            graph.AddRasterPass<int>("overlay", [=](int&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, target, {.Clear = {{.5f, .5f, .5f, 1}}}); }, nullptr);
            output.Texture = target;
        }
    }
};

/// Reads the swapchain output after the default composer finished its export blit.
class OutputReadbackComposer final : public FrameGraphComposer {
public:
    struct Flight {
        RgReadbackTicket Ticket;
        uint64_t Pitch{0};
        bool Bgra{false};
    };
    OutputReadbackComposer(RenderSystem& renderer, uint32_t flights) : Renderer(renderer), Flights(flights) {}
    void Compose(FrameGraph& frame, Nullable<RenderPipeline*> pipeline) override {
        const auto exported = ComposeDefaultFrameGraph(frame, pipeline, Renderer.GetOverlays(), Renderer);
        auto& flight = Flights[frame.Context.FlightIndex()];
        flight = {};
        for (const auto& surface : frame.Context.OutputSurfaces()) {
            const auto value = std::find_if(exported.begin(), exported.end(), [&](const auto& binding) { return binding.Output == surface.Id; });
            ASSERT_NE(value, exported.end());
            flight.Pitch = Align(uint64_t(surface.Desc.Width) * 4, frame.Context.Capabilities().Detail.TextureDataPitchAlignment);
            flight.Bgra = surface.Desc.Format == render::TextureFormat::BGRA8_UNORM || surface.Desc.Format == render::TextureFormat::BGRA8_UNORM_SRGB;
            flight.Ticket = frame.Graph.ReadbackTexture("presented", value->Texture);
        }
    }
    RenderSystem& Renderer;
    vector<Flight> Flights;
};

class OverlayApp final : public Application {
public:
    uint32_t Verified{0};
    uint8_t Sample{0};

protected:
    void OnInit() override {
        EXPECT_TRUE(GetRenderSystem()->AddOverlay(Overlay));
        EXPECT_FALSE(GetRenderSystem()->AddOverlay(Overlay));
        EXPECT_EQ(GetRenderSystem()->GetOverlays().size(), 1u);
        auto composer = make_unique<OutputReadbackComposer>(*GetRenderSystem(), GetGpuSystem()->GetFlightDataCount());
        Composer = composer.get();
        ASSERT_TRUE(GetRenderSystem()->SetGraphComposer(std::move(composer)));
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (++_updates >= 8) test::CloseMainWindow(*this);
    }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        if (!Composer || !completion.GpuWorkCompleted) return;
        auto& flight = Composer->Flights[completion.FlightIndex];
        if (!flight.Ticket.IsValid()) return;
        vector<byte> pixels;
        ASSERT_TRUE(flight.Ticket.Read(pixels));
        const auto offset = 8 * flight.Pitch + 8 * 4;
        ASSERT_LT(offset + 3, pixels.size());
        Sample = uint8_t(pixels[offset + (flight.Bgra ? 2 : 0)]);
        ++Verified;
        flight = {};
    }
    void OnShutdown() override {
        Composer = nullptr;
        ASSERT_TRUE(GetRenderSystem()->SetGraphComposer(nullptr));
        EXPECT_TRUE(GetRenderSystem()->RemoveOverlay(Overlay));
        EXPECT_FALSE(GetRenderSystem()->RemoveOverlay(Overlay));
    }

private:
    SolidOverlay Overlay;
    Nullable<OutputReadbackComposer*> Composer{nullptr};
    uint32_t _updates{0};
};

void RunOverlay(render::TextureFormat format, uint8_t expected) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, device)) GTEST_SKIP() << device.Reason;
    }
    test::RuntimeLogCapture logs;
    OverlayApp app;
    ASSERT_EQ(app.Run({.Backend = render::RenderBackend::D3D12, .EnableValidation = true, .WindowTitle = "overlay encoding", .WindowWidth = 64, .WindowHeight = 48, .FlightDataCount = 2, .BackBufferFormat = format, .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    EXPECT_GT(app.Verified, 0u);
    EXPECT_NEAR(int(app.Sample), int(expected), 2);
}

// Linear 0.5 written by the overlay: UNORM outputs receive explicit sRGB encoding (~188);
// sRGB attachments encode in hardware, so the stored bytes are the same encoded value.
TEST(RenderSystemOverlay, DefaultComposerEncodesUnormOutputsOnce) { RunOverlay(render::TextureFormat::BGRA8_UNORM, 188); }
TEST(RenderSystemOverlay, DefaultComposerDoesNotDoubleEncodeSrgbOutputs) { RunOverlay(render::TextureFormat::BGRA8_UNORM_SRGB, 188); }

}  // namespace
}  // namespace radray
