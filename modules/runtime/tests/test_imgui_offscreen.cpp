#include <array>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

#include <gtest/gtest.h>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include <radray/logger.h>
#ifdef RADRAY_ENABLE_PNG
#include <radray/image_data.h>
#endif
#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>

using namespace radray;
using namespace radray::render;

namespace {

constexpr uint32_t kWindowWidth = 1280;
constexpr uint32_t kWindowHeight = 720;
constexpr uint32_t kBackBufferCount = 2;
constexpr uint32_t kFlightFrameCount = 2;
constexpr uint32_t kMailboxCount = 1;
constexpr uint32_t kMailboxSlot = 0;
constexpr std::array<TextureFormat, 2> kFallbackFormats = {
    TextureFormat::BGRA8_UNORM,
    TextureFormat::RGBA8_UNORM};
constexpr std::array<VulkanCommandQueueDescriptor, 1> kVulkanQueueDescs = {
    VulkanCommandQueueDescriptor{QueueType::Direct, 1}};

class LogCollector {
public:
    static void Callback(LogLevel level, std::string_view message, void* userData) {
        auto* self = static_cast<LogCollector*>(userData);
        if (self == nullptr || (level != LogLevel::Err && level != LogLevel::Critical)) {
            return;
        }
        std::lock_guard<std::mutex> lock{self->_mutex};
        self->_errors.emplace_back(message);
    }

    vector<string> GetErrors() const {
        std::lock_guard<std::mutex> lock{_mutex};
        return _errors;
    }

    size_t GetErrorCount() const noexcept {
        std::lock_guard<std::mutex> lock{_mutex};
        return _errors.size();
    }

    void Truncate(size_t count) noexcept {
        std::lock_guard<std::mutex> lock{_mutex};
        if (_errors.size() > count) {
            _errors.resize(count);
        }
    }

private:
    mutable std::mutex _mutex;
    vector<string> _errors;
};

class ScopedGlobalLogCallback {
public:
    explicit ScopedGlobalLogCallback(LogCollector* logs) noexcept {
        SetLogCallback(&LogCollector::Callback, logs);
    }

    ~ScopedGlobalLogCallback() noexcept {
        ClearLogCallback();
    }
};

string JoinErrors(const vector<string>& errors, size_t maxCount = 8) {
    if (errors.empty()) {
        return {};
    }
    const size_t count = std::min(maxCount, errors.size());
    string result = fmt::format("{}", fmt::join(errors.begin(), errors.begin() + count, "\n"));
    if (errors.size() > count) {
        result += fmt::format("\n...({} more)", errors.size() - count);
    }
    return result;
}

std::string_view BackendTestName(RenderBackend backend) noexcept {
    switch (backend) {
        case RenderBackend::D3D12: return "D3D12";
        case RenderBackend::Vulkan: return "Vulkan";
        default: return "Unknown";
    }
}

vector<RenderBackend> GetEnabledRuntimeBackends() noexcept {
    vector<RenderBackend> backends;
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.push_back(RenderBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    backends.push_back(RenderBackend::Vulkan);
#endif
    return backends;
}

NativeWindowCreateDescriptor MakeWindowDesc(RenderBackend backend) {
#if defined(_WIN32)
    Win32WindowCreateDescriptor desc{};
    desc.Title = backend == RenderBackend::D3D12 ? "ImGuiOffscreen-D3D12" : "ImGuiOffscreen-Vulkan";
    desc.Width = static_cast<int32_t>(kWindowWidth);
    desc.Height = static_cast<int32_t>(kWindowHeight);
    desc.X = 180;
    desc.Y = 180;
    desc.Resizable = false;
    desc.StartMaximized = false;
    desc.Fullscreen = false;
    return desc;
#else
    RADRAY_UNUSED(backend);
    return Win32WindowCreateDescriptor{};
#endif
}

DeviceDescriptor MakeDeviceDesc(RenderBackend backend, LogCollector* logs) {
    switch (backend) {
        case RenderBackend::D3D12: {
            D3D12DeviceDescriptor desc{};
            desc.AdapterIndex = std::nullopt;
            desc.IsEnableDebugLayer = true;
            desc.IsEnableGpuBasedValid = true;
            desc.LogCallback = &LogCollector::Callback;
            desc.LogUserData = logs;
            return desc;
        }
        case RenderBackend::Vulkan: {
            VulkanDeviceDescriptor desc{};
            desc.PhysicalDeviceIndex = std::nullopt;
            desc.Queues = kVulkanQueueDescs;
            return desc;
        }
        default:
            RADRAY_ABORT("unsupported ImGui offscreen backend");
            return MetalDeviceDescriptor{};
    }
}

std::optional<VulkanInstanceDescriptor> MakeVulkanInstanceDesc(RenderBackend backend, LogCollector* logs) {
    if (backend != RenderBackend::Vulkan) {
        return std::nullopt;
    }
    VulkanInstanceDescriptor desc{};
    desc.AppName = "ImGuiOffscreen";
    desc.AppVersion = 1;
    desc.EngineName = "RadRay";
    desc.EngineVersion = 1;
    desc.IsEnableDebugLayer = true;
    desc.IsEnableGpuBasedValid = false;
    desc.LogCallback = &LogCollector::Callback;
    desc.LogUserData = logs;
    return desc;
}

class ImGuiOffscreenApp final : public Application {
public:
    void OnInitialize() override {}
    void OnUpdate() override {}
    void OnPrepareRender(AppWindow*, uint32_t) override {}
    void OnRender(AppWindow*, GpuFrameContext*, uint32_t) override {}
    void OnSubmit(AppWindow*, uint32_t, const GpuTask&) noexcept override {}
};

class ImGuiOffscreenTest : public testing::TestWithParam<RenderBackend> {};

TEST_P(ImGuiOffscreenTest, RenderDemoWindowToOffscreenTexture) {
    const RenderBackend backend = GetParam();
    LogCollector logs;
    ScopedGlobalLogCallback globalLogs{&logs};
    ImGuiOffscreenApp app;

    try {
        app.CreateGpuRuntime(MakeDeviceDesc(backend, &logs), MakeVulkanInstanceDesc(backend, &logs));
    } catch (const std::exception& ex) {
        GTEST_SKIP() << fmt::format("GpuRuntime create failed for {}: {}", BackendTestName(backend), ex.what());
    }
    ASSERT_NE(app._gpu, nullptr);
    ASSERT_TRUE(app._gpu->IsValid());

    AppWindow* window = nullptr;
    TextureFormat surfaceFormat = TextureFormat::UNKNOWN;
    string lastFailure = "CreateWindow failed";
    for (size_t i = 0; i < kFallbackFormats.size(); ++i) {
        const size_t errorBaseline = logs.GetErrorCount();
        try {
            GpuSurfaceDescriptor surfaceDesc{};
            surfaceDesc.Width = kWindowWidth;
            surfaceDesc.Height = kWindowHeight;
            surfaceDesc.BackBufferCount = kBackBufferCount;
            surfaceDesc.FlightFrameCount = kFlightFrameCount;
            surfaceDesc.Format = kFallbackFormats[i];
            surfaceDesc.PresentMode = PresentMode::FIFO;
            surfaceDesc.QueueSlot = 0;
            window = app.CreateWindow(MakeWindowDesc(backend), surfaceDesc, true, kMailboxCount);
            surfaceFormat = kFallbackFormats[i];
            break;
        } catch (const std::exception& ex) {
            lastFailure = fmt::format("CreateWindow failed for {}: {}", kFallbackFormats[i], ex.what());
        }
        if (i + 1 < kFallbackFormats.size()) {
            logs.Truncate(errorBaseline);
        }
    }
    if (window == nullptr) {
        GTEST_SKIP() << lastFailure;
    }

    unique_ptr<ImGuiSystem> imgui;
    {
        ImGuiSystemDescriptor imguiDesc{};
        imguiDesc.App = &app;
        imguiDesc.MainWnd = window;
        auto imguiOpt = ImGuiSystem::Create(imguiDesc);
        ASSERT_TRUE(imguiOpt.HasValue());
        imgui = imguiOpt.Release();
    }

    imgui->_context->SetCurrent();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(kWindowWidth), static_cast<float>(kWindowHeight));
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.IniFilename = nullptr;
    imgui->NewFrame();
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(32.0f, 32.0f),
        ImVec2(256.0f, 160.0f),
        IM_COL32(255, 64, 32, 255));
#ifndef IMGUI_DISABLE_DEMO_WINDOWS
    ImGui::SetNextWindowPos(ImVec2(270.0f, 20.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(330.0f, 320.0f), ImGuiCond_Always);
    bool showDemo = true;
    ImGui::ShowDemoWindow(&showDemo);
#endif
    ImGui::SetNextWindowPos(ImVec2(64.0f, 190.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(240.0f, 100.0f), ImGuiCond_Always);
    ImGui::Begin("ImGui Offscreen Smoke", nullptr, ImGuiWindowFlags_NoCollapse);
    ImGui::TextUnformatted("RadRay ImGui offscreen smoke frame");
    ImGui::Button("Visible Button", ImVec2(140.0f, 0.0f));
    ImGui::End();
    auto mailboxSlot = window->AllocMailboxSlot();
    ASSERT_TRUE(mailboxSlot.has_value());
    ASSERT_EQ(*mailboxSlot, kMailboxSlot);
    imgui->PrepareRenderData(window, *mailboxSlot);
    window->PublishPreparedMailbox(*mailboxSlot);
    auto queuedRequest = window->TryQueueLatestPublished();
    ASSERT_TRUE(queuedRequest.has_value());
    auto renderRequest = window->TryClaimQueuedRenderRequest();
    ASSERT_TRUE(renderRequest.has_value());
    ASSERT_EQ(renderRequest->MailboxSlot, *mailboxSlot);

    bool snapshotHasGeometry = false;
    if (!imgui->_viewportRendererData.empty() &&
        imgui->_viewportRendererData[0]->Mailboxes.size() > renderRequest->MailboxSlot) {
        const ImGuiRenderSnapshot& snapshot = imgui->_viewportRendererData[0]->Mailboxes[renderRequest->MailboxSlot];
        snapshotHasGeometry = snapshot.Valid && snapshot.TotalVtxCount > 0 && snapshot.TotalIdxCount > 0;
    }

    TextureDescriptor targetDesc{};
    targetDesc.Dim = TextureDimension::Dim2D;
    targetDesc.Width = kWindowWidth;
    targetDesc.Height = kWindowHeight;
    targetDesc.DepthOrArraySize = 1;
    targetDesc.MipLevels = 1;
    targetDesc.SampleCount = 1;
    targetDesc.Format = surfaceFormat;
    targetDesc.Memory = MemoryType::Device;
    targetDesc.Usage = TextureUse::RenderTarget;
#ifdef RADRAY_ENABLE_PNG
    targetDesc.Usage |= TextureUse::CopySource;
#endif
    GpuTextureHandle target = app._gpu->CreateTexture(targetDesc);
    ASSERT_TRUE(target.IsValid());
    auto* targetTexture = static_cast<Texture*>(target.NativeHandle);
    ASSERT_NE(targetTexture, nullptr);

#ifdef RADRAY_ENABLE_PNG
    const uint64_t tightReadbackRowPitch = static_cast<uint64_t>(kWindowWidth) * 4;
    const uint64_t readbackRowPitch = Align(tightReadbackRowPitch, std::max<uint64_t>(1, app._gpu->GetDevice()->GetDetail().TextureDataPitchAlignment));
    const uint64_t readbackByteSize = readbackRowPitch * kWindowHeight;
    GpuBufferHandle readback = app._gpu->CreateBuffer(BufferDescriptor{
        .Size = readbackByteSize,
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::MapRead | BufferUse::CopyDestination,
    });
    ASSERT_TRUE(readback.IsValid());
    auto* readbackBuffer = static_cast<Buffer*>(readback.NativeHandle);
    ASSERT_NE(readbackBuffer, nullptr);
#endif

    GpuTextureViewDescriptor targetViewDesc{};
    targetViewDesc.Target = target;
    targetViewDesc.Dim = TextureDimension::Dim2D;
    targetViewDesc.Format = surfaceFormat;
    targetViewDesc.Range = SubresourceRange{0, 1, 0, 1};
    targetViewDesc.Usage = TextureViewUsage::RenderTarget;
    GpuTextureViewHandle targetView = app._gpu->CreateTextureView(targetViewDesc);
    ASSERT_TRUE(targetView.IsValid());
    auto* targetRtv = static_cast<TextureView*>(targetView.NativeHandle);
    ASSERT_NE(targetRtv, nullptr);

    auto context = app._gpu->BeginAsync(QueueType::Direct, 0);
    ASSERT_NE(context, nullptr);
    auto* cmd = context->CreateCommandBuffer();
    ASSERT_NE(cmd, nullptr);
    cmd->Begin();
    imgui->Upload(window, renderRequest->MailboxSlot, context.get(), cmd);

    ResourceBarrierDescriptor toRenderTarget = BarrierTextureDescriptor{
        .Target = targetTexture,
        .Before = TextureState::Undefined,
        .After = TextureState::RenderTarget,
    };
    cmd->ResourceBarrier(std::span{&toRenderTarget, 1});

    ColorAttachment color{};
    color.Target = targetRtv;
    color.Load = LoadAction::Clear;
    color.Store = StoreAction::Store;
    color.ClearValue = ColorClearValue{{{0.0f, 0.0f, 0.0f, 1.0f}}};
    RenderPassDescriptor passDesc{};
    passDesc.ColorAttachments = std::span{&color, 1};
    passDesc.Name = "imgui-offscreen-smoke";
    auto passOpt = cmd->BeginRenderPass(passDesc);
    ASSERT_TRUE(passOpt.HasValue());
    auto pass = passOpt.Release();
    imgui->Render(window, renderRequest->MailboxSlot, pass.get());
    cmd->EndRenderPass(std::move(pass));

#ifdef RADRAY_ENABLE_PNG
    ResourceBarrierDescriptor toCopySource = BarrierTextureDescriptor{
        .Target = targetTexture,
        .Before = TextureState::RenderTarget,
        .After = TextureState::CopySource,
    };
    cmd->ResourceBarrier(std::span{&toCopySource, 1});
    if (backend == RenderBackend::Vulkan) {
        ResourceBarrierDescriptor toCopyDestination = BarrierBufferDescriptor{
            .Target = readbackBuffer,
            .Before = BufferState::Common,
            .After = BufferState::CopyDestination,
        };
        cmd->ResourceBarrier(std::span{&toCopyDestination, 1});
    }
    cmd->CopyTextureToBuffer(readbackBuffer, 0, targetTexture, SubresourceRange{0, 1, 0, 1});
    if (backend == RenderBackend::Vulkan) {
        ResourceBarrierDescriptor toHostRead = BarrierBufferDescriptor{
            .Target = readbackBuffer,
            .Before = BufferState::CopyDestination,
            .After = BufferState::HostRead,
        };
        cmd->ResourceBarrier(std::span{&toHostRead, 1});
    }
    ResourceBarrierDescriptor toCommon = BarrierTextureDescriptor{
        .Target = targetTexture,
        .Before = TextureState::CopySource,
        .After = TextureState::Common,
    };
#else
    ResourceBarrierDescriptor toCommon = BarrierTextureDescriptor{
        .Target = targetTexture,
        .Before = TextureState::RenderTarget,
        .After = TextureState::Common,
    };
#endif
    cmd->ResourceBarrier(std::span{&toCommon, 1});
    cmd->End();

    GpuTask task = app._gpu->SubmitAsync(std::move(context));
    ASSERT_TRUE(task.IsValid());
    GpuTask waitTask = task;
    window->SubmitPreparedResources(*renderRequest, task);
    imgui->OnSubmit(window, renderRequest->MailboxSlot, task);
    window->EndPrepareRenderTask(*renderRequest, std::move(task));
    waitTask.Wait();
    app._gpu->ProcessTasks();
    window->CollectCompletedFlightSlots();

    bool hasVisiblePixel = false;
#ifdef RADRAY_ENABLE_PNG
    ImageData image{};
    image.Width = kWindowWidth;
    image.Height = kWindowHeight;
    image.Format = ImageFormat::RGBA8_BYTE;
    image.Data = make_unique<byte[]>(image.GetSize());
    void* mapped = readbackBuffer->Map(0, readbackByteSize);
    ASSERT_NE(mapped, nullptr);
    const auto* src = static_cast<const byte*>(mapped);
    for (uint32_t y = 0; y < kWindowHeight; ++y) {
        const byte* srcRow = src + readbackRowPitch * y;
        byte* dstRow = image.Data.get() + tightReadbackRowPitch * y;
        if (surfaceFormat == TextureFormat::BGRA8_UNORM) {
            for (uint32_t x = 0; x < kWindowWidth; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 4 + 2];
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 4 + 0];
                dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
            }
        } else {
            ASSERT_EQ(surfaceFormat, TextureFormat::RGBA8_UNORM);
            std::memcpy(dstRow, srcRow, static_cast<size_t>(tightReadbackRowPitch));
        }
    }
    readbackBuffer->Unmap(0, readbackByteSize);
    for (size_t i = 0; i < image.GetSize(); i += 4) {
        if (image.Data[i + 0] != byte{0} || image.Data[i + 1] != byte{0} || image.Data[i + 2] != byte{0}) {
            hasVisiblePixel = true;
            break;
        }
    }
    const std::filesystem::path pngPath =
        std::filesystem::current_path() / fmt::format("imgui_offscreen_{}.png", BackendTestName(backend));
    const string pngPathString = pngPath.string();
    ASSERT_TRUE(image.WritePNG({pngPathString, false})) << fmt::format("failed to write {}", pngPathString);
#endif

    imgui.reset();
    app.WaitWindowTasks();
    app.OnShutdown();

    const auto errors = logs.GetErrors();
    EXPECT_TRUE(errors.empty()) << fmt::format("Captured render errors:\n{}", JoinErrors(errors));
    EXPECT_TRUE(snapshotHasGeometry);
#ifdef RADRAY_ENABLE_PNG
    EXPECT_TRUE(hasVisiblePixel);
#endif
}

INSTANTIATE_TEST_SUITE_P(
    RuntimeBackends,
    ImGuiOffscreenTest,
    testing::ValuesIn(GetEnabledRuntimeBackends()),
    [](const testing::TestParamInfo<RenderBackend>& info) {
        return string{BackendTestName(info.param)};
    });

}  // namespace
