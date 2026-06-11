#include <radray/runtime/application.h>
#include <radray/runtime/imgui_system.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/window_system.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material.h>
#include <radray/runtime/vertex_factory.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/renderer/scene.h>
#include <radray/runtime/renderer/scene_renderer.h>
#include <radray/render/common.h>
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

    // Build the sphere StaticMesh asset (CPU + GPU), register it with the AssetManager,
    // upload its GPU buffers via the framework's ResourceUploader, then spawn an Actor with
    // a StaticMeshComponent so the World/Scene drives rendering. No hand-written GPU mesh.
    void InitScene() {
        // ── Load the Material asset (ctor compiles shaders + acquires a shared root signature) ──
        std::filesystem::path shaderPath = std::filesystem::path{RADRAY_EXAMPLE_ASSET_DIR} / "sphere.hlsl";
        MaterialDescriptor matDesc{};
        matDesc.ShaderPath = shaderPath;
        matDesc.ShaderName = "sphere";
        matDesc.VsEntry = "VSMain";
        matDesc.PsEntry = "PSMain";
        // Draw both faces: winding/NDC conventions differ between D3D12 and Vulkan, and the
        // depth buffer resolves occlusion regardless of cull mode.
        matDesc.Primitive = render::PrimitiveState::Default();
        matDesc.Primitive.Cull = render::CullMode::None;
        matDesc.DepthStencil = render::DepthStencilState::Default();
        matDesc.DepthStencil.Format = DepthFormat;
        const AssetId matId = Guid::Parse("7f1d3b2a-0000-4000-8000-00000000b001");
        AssetRef<Material> matRef = _assetManager->Load<Material>(matId, *_renderSystem, matDesc);
        if (!matRef || !matRef->IsValid()) {
            RADRAY_ERR_LOG("failed to load sphere Material asset");
            return;
        }
        // Sanity-check that the material exposes the per-object constant slot the renderer expects.
        if (!matRef->FindParameterId("gScene").has_value()) {
            RADRAY_ERR_LOG("sphere material is missing gScene push constant");
            return;
        }

        // ── Build CPU mesh data and wrap it in a StaticMesh asset (goes through AssetManager) ──
        TriangleMesh tri{};
        tri.InitAsUVSphere(1.0f, 64);
        MeshResource meshResource{};
        tri.ToSimpleMeshResource(&meshResource);
        meshResource.Name = "sphere";
        if (meshResource.Primitives.empty()) {
            RADRAY_ERR_LOG("failed to build sphere mesh resource");
            return;
        }


        // Register the StaticMesh asset. AssetManager owns the asset lifetime; the component
        // (and its SceneProxy) keep an AssetRef so the asset stays alive while in use. The App
        // holds no long-lived ref: meshRef/matRef are local to InitScene and released on return.
        const AssetId meshId = Guid::Parse("7f1d3b2a-0000-4000-8000-00000000a001");
        AssetRef<StaticMesh> meshRef = _assetManager->Load<StaticMesh>(meshId);
        if (!meshRef) {
            RADRAY_ERR_LOG("failed to load sphere StaticMesh asset");
            return;
        }
        meshRef->SetMeshResource(std::move(meshResource));
        if (!meshRef->IsValid()) {
            RADRAY_ERR_LOG("sphere StaticMesh is invalid");
            return;
        }

        // ── Spawn the Actor + StaticMeshComponent into the World ──
        // GPU upload is now data-driven via the SceneProxy lifecycle: the proxy starts in
        // Pending; the SceneRenderer's frame-top PrepareResources uploads its mesh and advances
        // it to Ready, after which it draws. The component only hands over asset refs; no
        // hand-written command buffer / submit / wait, and no render-command queue.
        Actor* actor = _world->SpawnActor<Actor>();
        _sphereMeshComp = actor->AddComponent<StaticMeshComponent>(meshRef, matRef);
        actor->SetRootComponent(_sphereMeshComp);

        // ── Spawn a camera Actor + CameraComponent ──
        // The camera is an independent SceneComponent; SceneView is now derived from its
        // world transform + projection params instead of hand-computed in RecordSphere.
        Actor* cameraActor = _world->SpawnActor<Actor>();
        _cameraComp = cameraActor->AddComponent<CameraComponent>();
        cameraActor->SetRootComponent(_cameraComp);
        // Sit at z=-3 looking toward +Z (origin), matching the previous LookAtLH setup.
        _cameraComp->SetWorldLocation(Eigen::Vector3f{0.0f, 0.0f, -3.0f});
        _cameraComp->SetPerspective(Radian(60.0f), 0.1f, 100.0f);

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

        // Build the SceneView from the camera component (View/Proj/ViewProj + viewport).
        SceneView sceneView{};
        if (_cameraComp != nullptr) {
            _cameraComp->FillSceneView(sceneView, width, height);
        }

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

        MeshPassProcessor::Config processorConfig{};
        processorConfig.Cache = &_renderSystem->GetPSOCache();
        processorConfig.RtFormats.ColorFormats = {BackBufferFormat};
        processorConfig.RtFormats.DepthFormat = DepthFormat;
        processorConfig.ObjectConstantsParam = "gScene";

        // Let the SceneRenderer run InitViews (collect visible) -> BasePass (collect/sort/record).
        // PSO/RootSignature binding, per-object constants and draw calls all happen inside.
        _sceneRenderer.Render(encoder.get(), *_world->GetScene(), sceneView, processorConfig);

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
        // 帧顶资源准备:在任何 RenderPass 之前、以裸 CommandBuffer 让 SceneRenderer 遍历场景代理,
        // 对 Pending 态代理经 ResourceUploader 上传 GPU 网格并推进到 Ready。copy 与本帧绘制同一
        // 提交,同帧即就绪;未就绪的代理 InitViews 会跳过,“就绪后才绘制”。
        if (_world != nullptr && _world->GetScene() != nullptr) {
            _sceneRenderer.PrepareResources(cmdBuffer, *_world->GetScene(), ctx.GetUploader());
        }
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

        InitAssetManager();
        InitWorld();
        InitScene();
    }

    int Shutdown(const AppShutdownContext& ctx) override {
        (void)ctx;
        _renderSystem->WaitAndCleanupCompletedFlights();
        _frames.clear();
        // Tear down the World first: destroying actors removes SceneProxies, which drop the
        // AssetRefs they hold on the mesh/material. ShutdownAssetManager then unloads every
        // remaining asset (its dtor force-calls OnUnload regardless of refcount), freeing the
        // GPU buffers before the device is destroyed.
        _sphereMeshComp = nullptr;
        ShutdownWorld();
        // Cached PSOs reference shared shaders + root signatures owned by the render system.
        // Clear the PSO cache before tearing the render system down (member dtor order already
        // destroys _psoCache before the shader/root-signature caches, this is just explicit).
        _renderSystem->GetPSOCache().Clear();
        ShutdownAssetManager();
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

        // Advance the sphere spin (radians/sec). Drive it through the component's transform
        // so the change flows: SetRelativeRotation -> OnTransformChanged -> proxy world matrix.
        _sphereSpin += ctx.DeltaTime.count();
        if (_sphereSpin > 2.0f * std::numbers::pi_v<float>) {
            _sphereSpin -= 2.0f * std::numbers::pi_v<float>;
        }
        if (_sphereMeshComp != nullptr) {
            _sphereMeshComp->SetRelativeRotation(
                Eigen::Quaternionf{Eigen::AngleAxisf(_sphereSpin, Eigen::Vector3f::UnitY())});
        }
        _world->Tick(ctx.DeltaTime.count());

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

    // Sphere scene state. Material (RootSignature + shaders) is an asset; PSOs live in the
    // framework PSOCache; SceneRenderer drives collect/sort/record of draw commands.
    SceneRenderer _sceneRenderer;
    bool _sphereReady{false};
    float _sphereSpin{0.0f};

    // Scene framework: AssetManager and World now live on the Application base
    // (engine-level shared asset repo + current world container). Kept here only:
    // pointers into the World (non-owning) used every frame — the camera for the
    // SceneView and the mesh component for the spin animation. Asset lifetime is
    // owned by the AssetManager + component AssetRefs, so the App holds no ref.
    StaticMeshComponent* _sphereMeshComp{nullptr};
    CameraComponent* _cameraComp{nullptr};
};

int main(int argc, char* argv[]) {
    ExampleApp app{argc, argv};
    return app.Run();
}
