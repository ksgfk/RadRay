#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/window_system.h>
#include <radray/render/common.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/render/shader_compiler/spvc.h>
#include <radray/basic_math.h>
#include <radray/triangle_mesh.h>
#include <radray/file.h>
#include <radray/logger.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <numbers>

#ifndef RADRAY_EXAMPLE_ASSET_DIR
#define RADRAY_EXAMPLE_ASSET_DIR "."
#endif

using namespace radray;

// Push-constant payload shared by the sphere VS. Layout matches SceneConstants in sphere.hlsl:
// two column-major float4x4 (Eigen is column-major, HLSL cbuffers pack matrices column-major),
// 128 bytes total which fits Vulkan's guaranteed push-constant minimum.
struct SphereConstantsGpu {
    float MVP[16];
    float Model[16];
};

class ExampleApp : public Application {
    struct FrameResource {
        AppWindow* Window{nullptr};
        render::TextureStates BackBufferState{render::TextureState::Undefined};
        render::Texture* BackBuffer{nullptr};
    };

    struct Frame {
        void Init(ExampleApp* app) {
            render::QueryPoolDescriptor queryDesc{
                .Type = render::QueryType::Timestamp,
                .Count = TimestampQueryCount,
                .DebugName = "ImGui Frame Timestamp Pool"};
            TimestampPool = app->_device->CreateQueryPool(queryDesc).Unwrap();

            render::BufferDescriptor readbackDesc{
                .Size = sizeof(uint64_t) * TimestampQueryCount,
                .Memory = render::MemoryType::ReadBack,
                .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead};
            TimestampReadback = app->_device->CreateBuffer(readbackDesc).Unwrap();
        }

        FrameResource& FindWindowFrameResource(AppWindow* window) {
            auto iter = std::ranges::find_if(WindowTargets, [window](const FrameResource& target) {
                return target.Window == window;
            });
            if (iter != WindowTargets.end()) {
                return *iter;
            }

            FrameResource target;
            target.Window = window;
            WindowTargets.emplace_back(std::move(target));
            return WindowTargets.back();
        }

        unique_ptr<render::QueryPool> TimestampPool;
        unique_ptr<render::Buffer> TimestampReadback;
        bool TimestampPending{false};
        vector<FrameResource> WindowTargets;

        // Per-flight depth buffer for the sphere pass. Owned per flight so it can be
        // recreated on resize safely: the runtime waits this flight's fence before reusing
        // the slot, so the previous depth texture is guaranteed idle.
        unique_ptr<render::Texture> DepthTex;
        unique_ptr<render::TextureView> DepthView;
        render::TextureStates DepthState{render::TextureState::Undefined};
        uint32_t DepthWidth{0};
        uint32_t DepthHeight{0};
    };

    struct ViewportRenderTarget {
        ImGuiSystem::ViewportWindow* ViewportWindow{nullptr};
        AppFrameTarget Target;
    };

public:
    static constexpr uint32_t BackBufferCount = 3;
    static constexpr uint32_t FlightDataCount = 2;
    static constexpr uint32_t TimestampQueryCount = 2;
    static constexpr render::TextureFormat BackBufferFormat = render::TextureFormat::BGRA8_UNORM;
    static constexpr render::TextureFormat DepthFormat = render::TextureFormat::D32_FLOAT;

    ExampleApp(int argc, char* argv[]) noexcept {
        _args.reserve(argc);
        for (int i = 0; i < argc; ++i) {
            _args.emplace_back(argv[i]);
        }
    }

    int Run() {
        Init();
        return StartLoop();
    }

    bool AcquireViewportWindow(AppFrameContext& ctx, ImGuiSystem::ViewportWindow* viewportWindow, vector<ViewportRenderTarget>& renderTargets) {
        if (viewportWindow == nullptr || viewportWindow->Viewport == nullptr || viewportWindow->Window == nullptr) {
            return false;
        }
        if ((viewportWindow->Viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0) {
            return false;
        }
        NativeWindow* window = viewportWindow->GetWindow();
        if (window != nullptr && window->IsMinimized()) {
            return false;
        }
        if (viewportWindow->GetSwapChain() == nullptr) {
            return false;
        }

        std::optional<AppFrameTarget> target = ctx.AcquireWindow(viewportWindow->Window);
        if (!target.has_value()) {
            return false;
        }

        renderTargets.emplace_back(ViewportRenderTarget{
            .ViewportWindow = viewportWindow,
            .Target = target.value()});
        return true;
    }

    void RecordViewportWindow(AppFrameContext& ctx, Frame& frame, ViewportRenderTarget& target, render::CommandBuffer* cmdBuffer) {
        FrameResource& targetResource = frame.FindWindowFrameResource(target.ViewportWindow->Window);
        render::Texture* backBuffer = target.Target.BackBuffer;
        if (targetResource.BackBuffer != backBuffer) {
            targetResource.BackBuffer = backBuffer;
            targetResource.BackBufferState = render::TextureState::Undefined;
        }

        render::ResourceBarrierDescriptor toRenderTarget = render::BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = targetResource.BackBufferState,
            .After = render::TextureState::RenderTarget};
        cmdBuffer->ResourceBarrier(std::span{&toRenderTarget, 1});

        // Render the sphere into the main viewport's backbuffer first. ImGui then draws
        // on top with a Load action so the sphere stays visible behind the UI.
        const bool isMainViewport = target.ViewportWindow->Viewport == ImGui::GetMainViewport();
        bool sphereDrawn = false;
        if (isMainViewport && _sphereReady) {
            render::TextureDescriptor bbDesc = backBuffer->GetDesc();
            RecordSphere(cmdBuffer, frame, target.Target.BackBufferView, bbDesc.Width, bbDesc.Height);
            sphereDrawn = true;
        }

        render::ColorAttachment colorAttachment{
            .Target = target.Target.BackBufferView,
            .Load = sphereDrawn ? render::LoadAction::Load : render::LoadAction::Clear,
            .Store = render::StoreAction::Store,
            .ClearValue = render::ColorClearValue{{0.08f, 0.10f, 0.14f, 1.0f}}};
        render::RenderPassDescriptor renderPassDesc{
            .ColorAttachments = std::span{&colorAttachment, 1},
            .Name = isMainViewport ? "Main ImGui Viewport" : "ImGui Viewport"};
        auto encoderOpt = cmdBuffer->BeginRenderPass(renderPassDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ABORT("failed to begin imgui render pass");
        }
        auto encoder = encoderOpt.Release();
        _imguiSystem->_renderer->OnRenderViewport(ctx.FlightIndex(), target.ViewportWindow->Viewport, encoder.get());
        cmdBuffer->EndRenderPass(std::move(encoder));

        render::ResourceBarrierDescriptor toPresent = render::BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = render::TextureState::RenderTarget,
            .After = render::TextureState::Present};
        cmdBuffer->ResourceBarrier(std::span{&toPresent, 1});
        targetResource.BackBufferState = render::TextureState::Present;
    }

    Nullable<unique_ptr<render::Shader>> CompileShader(
        std::string_view source,
        std::string_view entryPoint,
        render::ShaderStage stage) {
        const bool isSpirv = _device->GetBackend() == render::RenderBackend::Vulkan;
        render::DxcCompileParams params{};
        params.Code = source;
        params.EntryPoint = entryPoint;
        params.Stage = stage;
        params.SM = render::HlslShaderModel::SM60;
        params.IsOptimize = false;
        params.IsSpirv = isSpirv;
        auto outputOpt = _dxc->Compile(params);
        if (!outputOpt.has_value()) {
            RADRAY_ERR_LOG("failed to compile sphere shader entry '{}'", entryPoint);
            return nullptr;
        }
        auto output = std::move(outputOpt.value());

        render::ShaderReflectionDesc reflection{};
        render::ShaderBlobCategory category{};
        if (isSpirv) {
#ifdef RADRAY_ENABLE_SPIRV_CROSS
            auto reflOpt = render::ReflectSpirv(render::SpirvBytecodeView{
                .Data = output.Data,
                .EntryPointName = entryPoint,
                .Stage = stage});
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("failed to reflect SPIR-V sphere shader '{}'", entryPoint);
                return nullptr;
            }
            reflection = std::move(reflOpt.value());
            category = render::ShaderBlobCategory::SPIRV;
#else
            RADRAY_ERR_LOG("SPIR-V Cross reflection is not enabled in this build");
            return nullptr;
#endif
        } else {
            auto reflOpt = _dxc->GetShaderDescFromOutput(output.Refl);
            if (!reflOpt.has_value()) {
                RADRAY_ERR_LOG("failed to reflect DXIL sphere shader '{}'", entryPoint);
                return nullptr;
            }
            reflection = std::move(reflOpt.value());
            category = render::ShaderBlobCategory::DXIL;
        }

        render::ShaderDescriptor shaderDesc{};
        shaderDesc.Source = std::span<const byte>{output.Data.data(), output.Data.size()};
        shaderDesc.Category = category;
        shaderDesc.Stages = stage;
        shaderDesc.Reflection = std::move(reflection);
        return _device->CreateShader(shaderDesc);
    }

    void InitSphere() {
        auto dxcOpt = render::CreateDxc();
        if (!dxcOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create DXC; sphere will not be rendered");
            return;
        }
        _dxc = dxcOpt.Release();

        std::filesystem::path shaderPath = std::filesystem::path{RADRAY_EXAMPLE_ASSET_DIR} / "sphere.hlsl";
        auto sourceOpt = ReadTextFile(shaderPath);
        if (!sourceOpt.has_value()) {
            RADRAY_ERR_LOG("failed to read sphere shader at {}", shaderPath.string());
            return;
        }
        const string& source = sourceOpt.value();

        auto vsOpt = CompileShader(source, "VSMain", render::ShaderStage::Vertex);
        if (!vsOpt.HasValue()) {
            return;
        }
        _sphereVS = vsOpt.Release();
        auto psOpt = CompileShader(source, "PSMain", render::ShaderStage::Pixel);
        if (!psOpt.HasValue()) {
            return;
        }
        _spherePS = psOpt.Release();

        render::Shader* shaders[] = {_sphereVS.get(), _spherePS.get()};
        render::RootSignatureDescriptor rsDesc{};
        rsDesc.Shaders = std::span<render::Shader*>{shaders};
        auto rsOpt = _device->CreateRootSignature(rsDesc);
        if (!rsOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create sphere root signature");
            return;
        }
        _sphereRootSig = rsOpt.Release();
        auto sceneIdOpt = _sphereRootSig->FindParameterId("gScene");
        if (!sceneIdOpt.has_value()) {
            RADRAY_ERR_LOG("sphere root signature is missing gScene push constant");
            return;
        }
        _sphereSceneParamId = sceneIdOpt.value();

        // Build the sphere mesh: interleave position + normal into one vertex buffer.
        TriangleMesh mesh{};
        mesh.InitAsUVSphere(1.0f, 64);
        const uint32_t vertexCount = static_cast<uint32_t>(mesh.Positions.size());
        _sphereVertexStride = sizeof(float) * 6;
        _sphereIndexCount = static_cast<uint32_t>(mesh.Indices.size());

        vector<float> vertexData(static_cast<size_t>(vertexCount) * 6);
        for (uint32_t i = 0; i < vertexCount; ++i) {
            const Eigen::Vector3f& p = mesh.Positions[i];
            const Eigen::Vector3f& n = mesh.Normals[i];
            float* dst = vertexData.data() + static_cast<size_t>(i) * 6;
            dst[0] = p.x();
            dst[1] = p.y();
            dst[2] = p.z();
            dst[3] = n.x();
            dst[4] = n.y();
            dst[5] = n.z();
        }
        const uint64_t vbBytes = vertexData.size() * sizeof(float);
        const uint64_t ibBytes = mesh.Indices.size() * sizeof(uint32_t);

        // Upload-heap buffers keep the example simple: no staging copy or barriers needed,
        // and the data is uploaded once at init.
        render::BufferDescriptor vbDesc{
            .Size = vbBytes,
            .Memory = render::MemoryType::Upload,
            .Usage = render::BufferUse::Vertex | render::BufferUse::MapWrite};
        auto vbOpt = _device->CreateBuffer(vbDesc);
        if (!vbOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create sphere vertex buffer");
            return;
        }
        _sphereVB = vbOpt.Release();
        _sphereVB->SetDebugName("sphere_vb");
        {
            void* mapped = _sphereVB->Map(0, vbBytes);
            std::memcpy(mapped, vertexData.data(), vbBytes);
            _sphereVB->Unmap(0, vbBytes);
        }

        render::BufferDescriptor ibDesc{
            .Size = ibBytes,
            .Memory = render::MemoryType::Upload,
            .Usage = render::BufferUse::Index | render::BufferUse::MapWrite};
        auto ibOpt = _device->CreateBuffer(ibDesc);
        if (!ibOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create sphere index buffer");
            return;
        }
        _sphereIB = ibOpt.Release();
        _sphereIB->SetDebugName("sphere_ib");
        {
            void* mapped = _sphereIB->Map(0, ibBytes);
            std::memcpy(mapped, mesh.Indices.data(), ibBytes);
            _sphereIB->Unmap(0, ibBytes);
        }

        render::VertexElement vertexElems[] = {
            {0, "POSITION", 0, render::VertexFormat::FLOAT32X3, 0},
            {sizeof(float) * 3, "NORMAL", 0, render::VertexFormat::FLOAT32X3, 1}};
        render::VertexBufferLayout vbLayout{
            _sphereVertexStride,
            render::VertexStepMode::Vertex,
            vertexElems};

        auto rtState = render::ColorTargetState::Default(BackBufferFormat);
        auto depthState = render::DepthStencilState::Default();
        depthState.Format = DepthFormat;

        render::GraphicsPipelineStateDescriptor psoDesc{
            _sphereRootSig.get(),
            render::ShaderEntry{_sphereVS.get(), "VSMain"},
            render::ShaderEntry{_spherePS.get(), "PSMain"},
            std::span<const render::VertexBufferLayout>{&vbLayout, 1},
            render::PrimitiveState::Default(),
            depthState,
            render::MultiSampleState::Default(),
            std::span<const render::ColorTargetState>{&rtState, 1}};
        // Draw both faces: winding/NDC conventions differ between D3D12 and Vulkan, and the
        // depth buffer resolves occlusion regardless of cull mode.
        psoDesc.Primitive.Cull = render::CullMode::None;

        auto psoOpt = _device->CreateGraphicsPipelineState(psoDesc);
        if (!psoOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create sphere pipeline state");
            return;
        }
        _spherePso = psoOpt.Release();
        _sphereReady = true;
    }

    // Ensure the per-flight depth buffer matches the given size; recreate if needed.
    render::TextureView* EnsureDepthBuffer(Frame& frame, uint32_t width, uint32_t height) {
        if (frame.DepthTex != nullptr && frame.DepthWidth == width && frame.DepthHeight == height) {
            return frame.DepthView.get();
        }
        frame.DepthView.reset();
        frame.DepthTex.reset();
        frame.DepthState = render::TextureState::Undefined;

        render::TextureDescriptor texDesc{
            .Dim = render::TextureDimension::Dim2D,
            .Width = width,
            .Height = height,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleCount = 1,
            .Format = DepthFormat,
            .Memory = render::MemoryType::Device,
            .Usage = render::TextureUse::DepthStencilWrite,
            .Hints = render::ResourceHint::None};
        auto texOpt = _device->CreateTexture(texDesc);
        if (!texOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create sphere depth texture");
            return nullptr;
        }
        frame.DepthTex = texOpt.Release();
        frame.DepthTex->SetDebugName("sphere_depth");

        render::TextureViewDescriptor viewDesc{
            .Target = frame.DepthTex.get(),
            .Dim = render::TextureDimension::Dim2D,
            .Format = DepthFormat,
            .Range = render::SubresourceRange::AllSub(),
            .Usage = render::TextureViewUsage::DepthWrite};
        auto viewOpt = _device->CreateTextureView(viewDesc);
        if (!viewOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to create sphere depth view");
            frame.DepthTex.reset();
            return nullptr;
        }
        frame.DepthView = viewOpt.Release();
        frame.DepthWidth = width;
        frame.DepthHeight = height;
        return frame.DepthView.get();
    }

    // Record the sphere draw into the same render pass as ImGui, before ImGui's UI.
    // Returns true if the sphere pass set up a depth attachment (so it must be cleared).
    void RecordSphere(
        render::CommandBuffer* cmdBuffer,
        Frame& frame,
        render::TextureView* colorView,
        uint32_t width,
        uint32_t height) {
        if (!_sphereReady || width == 0 || height == 0) {
            return;
        }
        render::TextureView* depthView = EnsureDepthBuffer(frame, width, height);
        if (depthView == nullptr) {
            return;
        }

        render::ResourceBarrierDescriptor toDepthWrite = render::BarrierTextureDescriptor{
            .Target = frame.DepthTex.get(),
            .Before = frame.DepthState,
            .After = render::TextureState::DepthWrite};
        cmdBuffer->ResourceBarrier(std::span{&toDepthWrite, 1});
        frame.DepthState = render::TextureState::DepthWrite;

        // Build MVP. Eigen is column-major, so the raw float[16] uploaded to the push
        // constant matches HLSL's column-major matrix packing; mul(M, v) is then correct.
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
        model.block<3, 3>(0, 0) =
            Eigen::AngleAxisf(_sphereSpin, Eigen::Vector3f::UnitY()).toRotationMatrix();
        const Eigen::Matrix4f view = LookAtLH<float>(
            Eigen::Vector3f{0.0f, 0.0f, -3.0f},
            Eigen::Vector3f{0.0f, 0.0f, 0.0f},
            Eigen::Vector3f{0.0f, 1.0f, 0.0f});
        const Eigen::Matrix4f proj =
            PerspectiveLH<float>(Radian(60.0f), aspect, 0.1f, 100.0f);
        const Eigen::Matrix4f mvp = proj * view * model;

        SphereConstantsGpu sceneData{};
        std::memcpy(sceneData.MVP, mvp.data(), sizeof(sceneData.MVP));
        std::memcpy(sceneData.Model, model.data(), sizeof(sceneData.Model));

        render::ColorAttachment colorAttachment{
            .Target = colorView,
            .Load = render::LoadAction::Clear,
            .Store = render::StoreAction::Store,
            .ClearValue = render::ColorClearValue{{0.08f, 0.10f, 0.14f, 1.0f}}};
        render::DepthStencilAttachment depthAttachment{
            .Target = depthView,
            .DepthLoad = render::LoadAction::Clear,
            .DepthStore = render::StoreAction::Store,
            .StencilLoad = render::LoadAction::DontCare,
            .StencilStore = render::StoreAction::Discard,
            .ClearValue = render::DepthStencilClearValue{1.0f, uint8_t{0}}};
        render::RenderPassDescriptor passDesc{
            .ColorAttachments = std::span{&colorAttachment, 1},
            .DepthStencilAttachment = depthAttachment,
            .Name = "Sphere Pass"};
        auto encoderOpt = cmdBuffer->BeginRenderPass(passDesc);
        if (!encoderOpt.HasValue()) {
            RADRAY_ERR_LOG("failed to begin sphere render pass");
            return;
        }
        auto encoder = encoderOpt.Release();

        Viewport vp{
            .X = 0.0f,
            .Y = 0.0f,
            .Width = static_cast<float>(width),
            .Height = static_cast<float>(height),
            .MinDepth = 0.0f,
            .MaxDepth = 1.0f};
        if (_device->GetBackend() == render::RenderBackend::Vulkan) {
            vp.Y = static_cast<float>(height);
            vp.Height = -static_cast<float>(height);
        }
        encoder->SetViewport(vp);
        encoder->SetScissor(Rect{0, 0, width, height});

        encoder->BindRootSignature(_sphereRootSig.get());
        encoder->BindGraphicsPipelineState(_spherePso.get());
        render::VertexBufferView vbv{
            .Target = _sphereVB.get(),
            .Offset = 0,
            .Size = static_cast<uint64_t>(_sphereVertexStride) * (_sphereVB->GetDesc().Size / _sphereVertexStride)};
        encoder->BindVertexBuffer(std::span{&vbv, 1});
        render::IndexBufferView ibv{
            .Target = _sphereIB.get(),
            .Offset = 0,
            .Stride = sizeof(uint32_t)};
        encoder->BindIndexBuffer(ibv);
        encoder->PushConstants(_sphereSceneParamId, &sceneData, sizeof(sceneData));
        encoder->DrawIndexed(_sphereIndexCount, 1, 0, 0, 0);

        cmdBuffer->EndRenderPass(std::move(encoder));
    }

    void Render(AppFrameContext& ctx) override {
        if (_imguiSystem == nullptr || _imguiSystem->_renderer == nullptr) {
            return;
        }

        vector<ViewportRenderTarget> renderTargets;
        renderTargets.reserve(_imguiSystem->_viewportWindows.size());
        for (const unique_ptr<ImGuiSystem::ViewportWindow>& viewportWindow : _imguiSystem->_viewportWindows) {
            if (viewportWindow == nullptr) {
                continue;
            }
            AcquireViewportWindow(ctx, viewportWindow.get(), renderTargets);
        }
        if (renderTargets.empty()) {
            return;
        }

        Frame& frame = _frames[ctx.FlightIndex()];
        render::CommandBuffer* cmdBuffer = ctx.GetCommandBuffer();
        cmdBuffer->ResetQueryPool(frame.TimestampPool.get(), 0, TimestampQueryCount);
        cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
            .Pool = frame.TimestampPool.get(),
            .Stage = render::QueryPipelineStage::Top,
            .Index = 0});
        _imguiSystem->_renderer->OnRenderBegin(ctx.FlightIndex(), cmdBuffer);
        for (ViewportRenderTarget& target : renderTargets) {
            RecordViewportWindow(ctx, frame, target, cmdBuffer);
        }
        cmdBuffer->WriteTimestamp(render::QueryTimestampDescriptor{
            .Pool = frame.TimestampPool.get(),
            .Stage = render::QueryPipelineStage::Bottom,
            .Index = 1});
        // Vulkan needs explicit transitions around the readback copy; D3D12 READBACK heaps stay in COPY_DEST.
        if (_readbackNeedsBarrier) {
            render::ResourceBarrierDescriptor toCopyDst = render::BarrierBufferDescriptor{
                .Target = frame.TimestampReadback.get(),
                .Before = render::BufferState::Common,
                .After = render::BufferState::CopyDestination};
            cmdBuffer->ResourceBarrier(std::span{&toCopyDst, 1});
        }
        cmdBuffer->ResolveQueryData(render::QueryResolveDescriptor{
            .Pool = frame.TimestampPool.get(),
            .FirstIndex = 0,
            .Count = TimestampQueryCount,
            .Destination = frame.TimestampReadback.get(),
            .DestinationOffset = 0});
        if (_readbackNeedsBarrier) {
            render::ResourceBarrierDescriptor toHostRead = render::BarrierBufferDescriptor{
                .Target = frame.TimestampReadback.get(),
                .Before = render::BufferState::CopyDestination,
                .After = render::BufferState::HostRead};
            cmdBuffer->ResourceBarrier(std::span{&toHostRead, 1});
        }
        frame.TimestampPending = true;
    }

    void OnRenderComplete(const AppRenderCompleteContext& ctx) override {
        if (_imguiSystem != nullptr && _imguiSystem->_renderer != nullptr) {
            _imguiSystem->_renderer->OnRenderComplete(ctx.FlightIndex);
        }
        ResolveGpuTime(ctx.FlightIndex);
    }

    void ResolveGpuTime(uint32_t flightIndex) {
        Frame& frame = _frames[flightIndex];
        if (!frame.TimestampPending) {
            return;
        }
        frame.TimestampPending = false;

        const uint64_t mappedSize = sizeof(uint64_t) * TimestampQueryCount;
        void* mapped = frame.TimestampReadback->Map(0, mappedSize);
        if (mapped == nullptr) {
            return;
        }
        uint64_t ticks[TimestampQueryCount]{};
        std::memcpy(ticks, mapped, mappedSize);
        frame.TimestampReadback->Unmap(0, mappedSize);

        if (ticks[1] <= ticks[0]) {
            return;
        }
        const render::TimestampQueryCalibration calibration =
            frame.TimestampPool->GetTimestampCalibration(_renderSystem->_mainQueue);
        if (calibration.TickPeriodNs <= 0.0) {
            return;
        }
        const double elapsedNs = static_cast<double>(ticks[1] - ticks[0]) * calibration.TickPeriodNs;
        _gpuTimeMs = static_cast<float>(elapsedNs / 1'000'000.0);
    }

    void OnSwapChainRecreate(const AppSwapChainRecreateContext& ctx) override {
        if (_imguiSystem != nullptr && _imguiSystem->_renderer != nullptr) {
            _imguiSystem->_renderer->OnSwapChainRecreate(ctx);
        }
        auto window = ctx.Window;
        for (Frame& frame : _frames) {
            auto iter = std::ranges::find_if(frame.WindowTargets, [window](const FrameResource& target) {
                return target.Window == window;
            });
            if (iter == frame.WindowTargets.end()) {
                continue;
            }
            iter->BackBuffer = nullptr;
            iter->BackBufferState = render::TextureState::Undefined;
        }
    }

    void Init() {
        render::RenderBackend backend = render::RenderBackend::Vulkan;
        bool enableValid = false, multithread = false;
        for (size_t i = 0; i < _args.size(); ++i) {
            if (_args[i] == "--backend" && i + 1 < _args.size()) {
                const auto& backendStr = _args[i + 1];
                if (backendStr == "vulkan") {
                    backend = render::RenderBackend::Vulkan;
                } else if (backendStr == "d3d12") {
                    backend = render::RenderBackend::D3D12;
                }
            }
            if (_args[i] == "--valid-layer") {
                enableValid = true;
            }
            if (_args[i] == "--multithread") {
                multithread = true;
            }
        }
        _multithreaded = multithread;
        if (backend == render::RenderBackend::Vulkan) {
            render::VulkanInstanceDescriptor insDesc{
                .AppName = "Example ImGui App",
                .EngineName = "RadRay",
                .IsEnableDebugLayer = enableValid,
                .IsEnableGpuBasedValid = false};
            render::InstanceVulkan::InitEnv(insDesc).Unwrap();
            render::VulkanCommandQueueDescriptor queueDesc{render::QueueType::Direct, 1};
            render::VulkanDeviceDescriptor deviceDesc{
                .Queues = std::span{&queueDesc, 1}};
            _device = render::Device::Create(deviceDesc).Unwrap();
        } else if (backend == render::RenderBackend::D3D12) {
            render::DXGIFactoryDescriptor dxgiDesc{
                enableValid,
                enableValid};
            _dxgiFactory = render::DXGIFactory::Create(dxgiDesc).Unwrap();
            render::D3D12DeviceDescriptor deviceDesc{_dxgiFactory.get()};
            _device = render::Device::Create(deviceDesc).Unwrap();
        }
        _readbackNeedsBarrier = _device->GetBackend() == render::RenderBackend::Vulkan;
        AppWindowSystemDescriptor wndSysDesc{};
#ifdef RADRAY_PLATFORM_WINDOWS
        wndSysDesc.Type = NativeWindowType::Win32HWND;
#endif
        InitWindowSystem(wndSysDesc);

        AppRenderSystemDescriptor renderSysDesc{
            .Device = _device.get(),
            .MainQueueIndex = 0,
            .BackBufferCount = BackBufferCount,
            .FlightDataCount = FlightDataCount};
        InitRenderSystem(renderSysDesc);
        _frames.resize(renderSysDesc.FlightDataCount);
        for (Frame& frame : _frames) {
            frame.Init(this);
        }

#ifdef RADRAY_PLATFORM_WINDOWS
        Win32WindowCreateDescriptor wndDesc{
            .Title = "Example ImGui App",
            .Width = 1280,
            .Height = 720,
            .Resizable = true,
            .StartVisible = true};
        _mainWindow = _windowSystem->CreateWindow(wndDesc, true);
#else
        RADRAY_ABORT("unsupported platform");
#endif

        render::SwapChainDescriptor swapchainDesc{
            .Width = static_cast<uint32_t>(wndDesc.Width),
            .Format = BackBufferFormat,
            .PresentMode = render::PresentMode::FIFO};
        _mainWindow->AttachSwapChain(swapchainDesc);

        ImGuiSystemDescriptor imguiDesc{
            .MainWindow = _mainWindow,
            .WindowSystem = _windowSystem.get(),
            .Device = _device.get(),
            .RenderTargetFormat = BackBufferFormat,
            .FlightDataCount = renderSysDesc.FlightDataCount,
            .DirectQueue = _renderSystem->_mainQueue,
            .BackBufferCount = renderSysDesc.BackBufferCount,
            .PresentMode = render::PresentMode::FIFO};
        _imguiSystem = ImGuiSystem::Create(imguiDesc).Unwrap();

        InitSphere();
    }

    int Shutdown(const AppShutdownContext& ctx) override {
        (void)ctx;
        _renderSystem->WaitAndCleanupCompletedFlights();
        _frames.clear();
        _spherePso.reset();
        _sphereRootSig.reset();
        _spherePS.reset();
        _sphereVS.reset();
        _sphereVB.reset();
        _sphereIB.reset();
        _dxc.reset();
        _imguiSystem.reset();
        _mainWindow->DetachSwapChain();
        _mainWindow = nullptr;
        ShutdownRenderSystem();
        ShutdownWindowSystem();
        _device.reset();
        _dxgiFactory.reset();
        return 0;
    }

    AppUpdateResult Update(const AppUpdateContext& ctx) override {
        bool shouldClose = _mainWindow->_window->ShouldClose();
        if (shouldClose) {
            return AppUpdateResult{shouldClose};
        }

        // Advance the sphere spin (radians/sec) so the normals visibly rotate.
        _sphereSpin += ctx.DeltaTime.count();
        if (_sphereSpin > 2.0f * std::numbers::pi_v<float>) {
            _sphereSpin -= 2.0f * std::numbers::pi_v<float>;
        }

        bool imguiSucc = _imguiSystem->BeginFrame(ctx.FlightIndex, ctx.DeltaTime.count());
        if (imguiSucc) {
            ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration |
                                           ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav |
                                           ImGuiWindowFlags_NoMove;
            int location = 0;
            constexpr float PAD = 10.0f;
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImVec2 workPos = viewport->WorkPos;
            ImVec2 workSize = viewport->WorkSize;
            ImVec2 windowPos;
            ImVec2 windowPosPivot;
            windowPos.x = (location & 1) ? (workPos.x + workSize.x - PAD) : (workPos.x + PAD);
            windowPos.y = (location & 2) ? (workPos.y + workSize.y - PAD) : (workPos.y + PAD);
            windowPosPivot.x = (location & 1) ? 1.0f : 0.0f;
            windowPosPivot.y = (location & 2) ? 1.0f : 0.0f;
            ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPosPivot);
            ImGui::SetNextWindowBgAlpha(0.35f);
            if (ImGui::Begin("RadrayMonitor", &_showMonitor, windowFlags)) {
                ImGui::Text("Delta time: %06.3f ms", ctx.DeltaTime.count() * 1000.0f);
                ImGui::Text("Frame latency: %06.3f ms", ctx.LastFrameLatency.count() * 1000.0f);
                ImGui::Text("GPU time: %06.3f ms", _gpuTimeMs);
                static constexpr render::PresentMode kModes[] = {
                    render::PresentMode::FIFO,
                    render::PresentMode::Mailbox,
                    render::PresentMode::Immediate};
                render::PresentMode currentMode = render::PresentMode::FIFO;
                if (_mainWindow != nullptr && _mainWindow->_swapchain) {
                    currentMode = _mainWindow->_swapchain.Get()->GetDesc().PresentMode;
                }
                std::string preview{render::format_as(currentMode)};
                if (ImGui::BeginCombo("Present Mode", preview.c_str())) {
                    for (render::PresentMode mode : kModes) {
                        std::string item{render::format_as(mode)};
                        const bool selected = mode == currentMode;
                        if (ImGui::Selectable(item.c_str(), selected) && mode != currentMode) {
                            _windowSystem->SetPresentMode(mode);
                        }
                        if (selected) {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
            }
            ImGui::End();

            ImGui::ShowDemoWindow();
            _imguiSystem->EndFrame();
        }
        if (imguiSucc) {
            _imguiSystem->_renderer->ExtractDrawData(ctx.FlightIndex);
        }

        return AppUpdateResult{shouldClose};
    }

    vector<string> _args;
    unique_ptr<render::DXGIFactory> _dxgiFactory;
    shared_ptr<render::Device> _device;
    unique_ptr<ImGuiSystem> _imguiSystem;
    vector<Frame> _frames;

    AppWindow* _mainWindow{nullptr};
    bool _showMonitor{true};
    float _gpuTimeMs{0.0f};
    bool _readbackNeedsBarrier{false};

    // Sphere render resources (created once in Init).
    shared_ptr<render::Dxc> _dxc;
    unique_ptr<render::Shader> _sphereVS;
    unique_ptr<render::Shader> _spherePS;
    unique_ptr<render::RootSignature> _sphereRootSig;
    unique_ptr<render::GraphicsPipelineState> _spherePso;
    unique_ptr<render::Buffer> _sphereVB;
    unique_ptr<render::Buffer> _sphereIB;
    uint32_t _sphereIndexCount{0};
    uint32_t _sphereVertexStride{0};
    render::BindingParameterId _sphereSceneParamId{};
    bool _sphereReady{false};
    float _sphereSpin{0.0f};
};

int main(int argc, char* argv[]) {
    ExampleApp app{argc, argv};
    return app.Run();
}
