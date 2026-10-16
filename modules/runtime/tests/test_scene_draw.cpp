#include <radray/runtime/components/scene_view_capture.h>
#include <radray/runtime/render_framework/scene_manager.h>
#include "test_scene_draw.h"
#include "runtime_test_support.h"
#include "gpu_runtime_test_support.h"
#include "scene_draw.h"
#include <radray/runtime/render_framework/scene_draw.h>
#include <radray/runtime/world_manager.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/components/instanced_static_mesh_component.h>

namespace radray::test {

class SceneDrawApp final : public Application {
public:
    uint32_t Views{1};
    uint32_t Width() const noexcept { return std::max(192u, Views * (MaterialCase ? 128u : 64u)); }
    bool Drop{false};
    bool MaterialCase{false};
    bool DepthCase{false};
    bool Instanced{false};
    bool Manual{false};
    uint32_t Verified{0}, Dropped{0}, ZeroViews{0}, CameraOnlyVerified{0};

protected:
    void OnInit() override {
        auto* device = GetGpuSystem()->GetDevice();
        const auto worldId = GetWorldManager()->CreateWorld();
        _world = GetWorldManager()->GetWorld(worldId).Get();
        GetWorldManager()->RequestRenderConnection(worldId, true);
        auto cube = example::CreateCube(*GetGpuSystem().Get());
        ASSERT_TRUE(cube);
        if (MaterialCase) {
            auto cpu = cube->GetMeshResource();
            array<float, 32> interleaved{};
            const auto* positions = reinterpret_cast<const float*>(cpu.Bins[0].GetData().data());
            for (uint32_t i = 0; i < 8; ++i) std::copy_n(positions + i * 3, 3, interleaved.data() + i * 4 + 1);
            array<float, 16> uv;
            uv.fill(0.5f);
            cpu.Bins[0] = MeshBuffer(std::as_bytes(std::span{interleaved}));
            cpu.Bins.emplace_back(std::as_bytes(std::span{uv}));
            auto& primitive = cpu.Primitives[0];
            primitive.VertexBuffers[0].Stride = 16;
            primitive.VertexBuffers[0].Offset = 4;
            primitive.VertexBuffers.push_back({"TEXCOORD", 1, 2, VertexDataType::FLOAT, 2, 0, 8});
            auto storage = device->CreateCommandAllocator(GetGpuSystem()->GetMainQueue()).Unwrap();
            auto command = device->CreateCommandBuffer(storage.get()).Unwrap();
            StagingBufferPool staging{device};
            ResourceUploader uploader{device, staging};
            HostWriteBatch writes;
            staging.BeginEpoch(writes);
            command->Begin();
            auto geometry = uploader.UploadMeshResource(command.get(), cpu);
            command->End();
            staging.SealEpoch();
            ASSERT_TRUE(geometry);
            writes.Flush(*device);
            auto* ptr = command.get();
            GetGpuSystem()->GetMainQueue()->Submit({.CmdBuffers = std::span{&ptr, 1}});
            GetGpuSystem()->GetMainQueue()->Wait();
            staging.CompleteEpoch();
            cube = make_unique<StaticMesh>(std::move(cpu), vector<StaticMeshSection>{}, Eigen::Vector3f{-0.5f, -0.5f, -0.5f}, Eigen::Vector3f{0.5f, 0.5f, 0.5f}, std::move(*geometry));
        }
        _mesh = GetAssetManager()->AddReady<StaticMesh>(AssetId{19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, std::move(cube));
        if (Manual) {
            _manual = _world->SpawnActor()->AddComponent<InstancedStaticMeshComponent>();
            _manual->SetStaticMesh(_mesh);
            array<InstanceInit, 2> initial{};
            initial[1].LocalToComponent(0, 3) = 1.5f;
            initial[1].LocalToComponent(1, 3) = .45f;
            initial[1].LocalToComponent(2, 3) = 4;
            const size_t count = MaterialCase ? 2 : 1;
            ASSERT_TRUE(_manual->AddInstances(std::span{initial}.first(count), std::span{_handles}.first(count)));
        } else {
            _object = _world->SpawnActor()->AddComponent<StaticMeshComponent>();
            _object->SetStaticMesh(_mesh);
        }
        _cameraActor = _world->SpawnActor();
        _camera = _cameraActor->AddComponent<CameraComponent>();
        if (MaterialCase) {
            _camera->SetOrthographic(4.0f, 0.1f, 100.0f);
            auto image = render::test::MakeRenderTarget(device, render::TextureFormat::RGBA8_UNORM, 1, 1, render::TextureUse::RenderTarget | render::TextureUse::Resource);
            ASSERT_TRUE(image);
            auto srvDesc = image->View->GetDesc();
            srvDesc.Usage = render::TextureViewUsage::Resource;
            auto srv = device->CreateTextureView(srvDesc).Unwrap();
            auto storage = device->CreateCommandAllocator(GetGpuSystem()->GetMainQueue()).Unwrap();
            auto command = device->CreateCommandBuffer(storage.get()).Unwrap();

            auto* view = image->View.get();

            command->Begin();
            RenderOutput output{{{view, render::TextureState::Undefined, render::TextureState::ShaderRead}}, {}};
            auto state = InitialRenderOutputState(output);
            TransitionRenderOutput(command.get(), output, state, false);
            const render::ColorClearValue white{{1, 1, 1, 1}};
            auto encoder = command->BeginRenderPass({array{render::RenderPassColorAttachment{view, render::LoadAction::Clear, render::StoreAction::Store, white}}, {}, 1, 1, 1});
            ASSERT_TRUE(encoder);
            command->EndRenderPass(encoder.Release());
            TransitionRenderOutput(command.get(), output, state, true);
            command->End();
            auto* ptr = command.get();
            GetGpuSystem()->GetMainQueue()->Submit({.CmdBuffers = std::span{&ptr, 1}});
            GetGpuSystem()->GetMainQueue()->Wait();
            auto texture = GetAssetManager()->AddReady<TextureAsset>({91, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<TextureAsset>(device, "material white", std::move(image->Tex), std::move(srv)));
            for (uint32_t i = 0; i < 2; ++i) {
                MaterialTemplateDescriptor material;
                material.Name = "Unlit";
                material.Program.SourceName = Instanced ? "material_unlit_instanced.hlsl" : "material_unlit.hlsl";
                material.Inputs = {{"Objects", MaterialInputSource::SceneObjects}, {"ViewData", MaterialInputSource::ViewConstants}, {"MaterialData", MaterialInputSource::MaterialConstants}, {"Albedo", MaterialInputSource::Texture}, {"AlbedoSampler", MaterialInputSource::Sampler}};
                material.ObjectIndexPushConstant = "DrawData";
                if (Instanced) {
                    material.ObjectIndexPushConstant.clear();
                    material.PushConstant = "DrawData";
                    material.SceneIndexField = "VisibleBase";
                    material.InstanceAddressing = MaterialInstanceAddressing::VisibleSlots;
                    material.AllowAutomaticInstancing = !Manual;
                    material.Inputs.push_back({"VisibleInstanceSlots", MaterialInputSource::VisibleInstanceSlots});
                }
                MaterialTemplateDescriptor unused = material;
                unused.Name = "Unused";
                unused.ParameterLayouts = {{"UnusedParameters", 16, {{"Value", 0, 16, 2, 1, 1, 4}}}};
                const array<float, 4> tint{1, 1, i ? 0.5f : 1.0f, 1};
                const auto bytes = std::as_bytes(std::span{tint});
                render::SamplerDescriptor sampler{};
                sampler.AddressS = sampler.AddressT = sampler.AddressR = render::AddressMode::ClampToEdge;
                sampler.MinFilter = sampler.MagFilter = sampler.MipmapFilter = render::FilterMode::Nearest;
                auto asset = make_unique<Material>(vector{material, unused}, vector<vector<byte>>{vector<byte>(bytes.begin(), bytes.end())}, vector{texture}, vector{sampler});
                ASSERT_TRUE(asset->IsValid());
                _materials.push_back(GetAssetManager()->AddReady<Material>({92 + i, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, std::move(asset)));
            }
            if (!Manual) {
                auto* second = _world->SpawnActor()->AddComponent<StaticMeshComponent>();
                second->SetStaticMesh(_mesh);
                second->SetMaterials({_materials[0]});
                second->SetRelativeLocation({1.5f, 0.45f, 4});
            }
        }
        _frames.resize(GetGpuSystem()->GetFlightDataCount());
        MaterialTemplateDescriptor materialPass;
        materialPass.Name = DepthCase ? "DepthOnly" : "Unlit";
        materialPass.Program.SourceName = DepthCase ? "material_depth.hlsl" : "scene_sync.hlsl";
        materialPass.Inputs = {{"Objects", MaterialInputSource::SceneObjects}, {"ViewData", MaterialInputSource::ViewConstants}};
        materialPass.ObjectIndexPushConstant = "DrawData";
        if (Instanced) {
            materialPass.Program.SourceName = DepthCase ? "material_depth_instanced.hlsl" : "scene_sync_instanced.hlsl";
            materialPass.ObjectIndexPushConstant.clear();
            materialPass.PushConstant = "DrawData";
            materialPass.SceneIndexField = "VisibleBase";
            materialPass.InstanceAddressing = MaterialInstanceAddressing::VisibleSlots;
            materialPass.AllowAutomaticInstancing = !Manual;
            materialPass.Inputs.push_back({"VisibleInstanceSlots", MaterialInputSource::VisibleInstanceSlots});
        }
        const auto definition = GetRenderer()->RegisterTemplate(materialPass);
        ASSERT_TRUE(definition) << GetRenderer()->GetLastError();
        const auto fallback = GetRenderer()->RegisterMaterial({{*definition}, {}, {}, {}});
        GetRenderer()->UnregisterTemplate(*definition);
        ASSERT_TRUE(fallback);
        _draw = make_unique<UnlitRenderPipeline>(ScenePipelineDescriptor{.PassName = DepthCase ? "DepthOnly" : "Unlit", .Fallback = std::move(fallback), .DepthTest = DepthCase, .ClearColor = {{0, 0, 0, 1}}});
        _pitch = Align(uint64_t{Width()} * 4, device->GetDetail().TextureDataPitchAlignment);
        for (auto& frame : _frames) {
            auto image = render::test::MakeRenderTarget(device, render::TextureFormat::RGBA8_UNORM, Width(), 96, render::TextureUse::RenderTarget | render::TextureUse::CopySource);
            ASSERT_TRUE(image);
            frame.Image = std::move(*image);
            if (DepthCase) {
                frame.Image.View.reset();
                frame.Image.Tex = device->CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = Width(), .Height = 96, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::D32_FLOAT, .Usage = render::TextureUse::DepthStencilWrite | render::TextureUse::CopySource}).Unwrap();
                frame.Image.View = device->CreateTextureView({frame.Image.Tex.get(), render::TextureDimension::Dim2D, render::TextureFormat::D32_FLOAT, {0, 1, 0, 1}, render::TextureViewUsage::DepthWrite}).Unwrap();
            }
            frame.Readback = device->CreateBuffer({.Size = _pitch * 96, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
        }
    }
    void OnUpdate(const AppUpdateContext& context) override {
        _updateFlight = context.FlightIndex;
        ++_updates;
        if (Drop && _updates == 2) {
            _oldShape = _object->GetShapeId();
            _world->DestroyActor(_object->GetOwner().Get());
            _object = nullptr;
        }
        if (Drop && _updates == 3) {
            _object = _world->SpawnActor()->AddComponent<StaticMeshComponent>();
            _object->SetStaticMesh(_mesh);
            EXPECT_EQ(_object->GetShapeId().Index, _oldShape.Index);
            EXPECT_NE(_object->GetShapeId().Generation, _oldShape.Generation);
        }
        if (_object && _updates <= 11) {
            _object->SetRelativeLocation({_updates % 2 ? -0.65f : 0.65f, 0.45f, 4});
            _object->SetRelativeScale({_updates % 3 ? 1.0f : -1.0f, 1, 1});
        }
        if (_manual && _updates <= 11) {
            Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
            matrix(0, 0) = _updates % 3 ? 1.0f : -1.0f;
            matrix.block<3, 1>(0, 3) = Eigen::Vector3f{_updates % 2 ? -.65f : .65f, .45f, 4};
            const InstanceUpdate update{_handles[0], matrix};
            ASSERT_TRUE(_manual->UpdateInstances(std::span{&update, 1}));
        }
        if (MaterialCase && _object) _object->SetMaterials({_materials[_updates % 2]});
        if (MaterialCase && _manual) _manual->SetMaterials({_materials[_updates % 2]});
        if (_camera) _camera->SetRelativeLocation({float(_updates % 3) * 0.15f, 0, 0});
        if (_updates == 8) {
            _world->DestroyActor(_cameraActor.Get());
            _camera = nullptr;
        }
        if (_updates == 9) {
            _world->RequestReconnect();
            _cameraActor = _world->SpawnActor();
            _camera = _cameraActor->AddComponent<CameraComponent>();
            if (MaterialCase) _camera->SetOrthographic(4.0f, 0.1f, 100.0f);
        }
        if (Drop && (_updates == 6 || _updates == 7)) {
            auto* window = GetWindowManager()->GetMainWindow();
            ::ShowWindow(static_cast<HWND>(window->GetNativeWindow()->GetNativeHandler()), _updates == 6 ? SW_MINIMIZE : SW_RESTORE);
            if (_updates == 7) _tasks.Spawn(Resize());
        }
        if (_updates >= 18) RequestExit();
    }
    void OnCollectRenderViews(SceneViewCollector& collector) override {
        if (!_camera || (!_object && !_manual) || _updates == 5) return;
        _frames[_updateFlight].ExpectedBlue = MaterialCase && _updates % 2 ? 89 : 179;
        _frames[_updateFlight].ExpectedView = _camera->ComputeViewMatrix();
        _frames[_updateFlight].ExpectedScene = *_world->GetRenderSceneId();
        _frames[_updateFlight].ExpectStationaryObjects = _updates >= 11 + GetGpuSystem()->GetFlightDataCount();
        for (uint32_t i = 0; i < Views; ++i) {
            const auto view = CaptureSceneView(*_camera.Get(), {float(i) / Views, 0, 1.0f / Views, 1});
            ASSERT_TRUE(view);
            ASSERT_TRUE(collector.Add(*view));
        }
    }
    void OnRender(AppFrameContext& context) override {
        std::optional<AppFrameTarget> target;
        if (Drop) {
            target = context.AcquireWindow(GetWindowManager()->GetMainWindow());
            if (!target) return;
        }
        auto* renderer = GetRenderSystem().Get();
        const auto views = GetSceneManager()->GetFrameViewsRT(context.FlightIndex());
        if (views.empty()) {
            ++ZeroViews;
            if (target) {
                auto* commands = context.AllocateCommandBuffer();
                const render::ResourceBarrierDescriptor present = render::BarrierTextureDescriptor{.Target = target->BackBuffer,
                                                                                                   .Before = target->Window->GetBackBufferState(target->BackBufferIndex),
                                                                                                   .After = render::TextureState::Present};
                commands->ResourceBarrier(std::span{&present, 1});
                context.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}, std::move(target));
            }
            return;
        }
        auto& frame = _frames[context.FlightIndex()];
        frame.Serial = context.FrameSerial();
        frame.Points.clear();
        frame.PointBlues.clear();
        auto builder = GetRenderer()->BeginFrame(context);
        ASSERT_TRUE(builder);
        vector<std::optional<SceneGpuView>> objects(views.size());
        const auto beforeBytes = context.GetHostWrites().GetStats().CommittedBytes;
        for (uint32_t i = 0; i < views.size(); ++i) {
            EXPECT_TRUE(views[i].View.isApprox(frame.ExpectedView));
            EXPECT_EQ(views[i].Scene, frame.ExpectedScene);
            objects[i] = GetRenderer()->GetScenes().Prepare(GetSceneManager()->GetRendererSceneKey(views[i].Scene), *GetSceneManager()->GetSceneRT(views[i].Scene), context, *builder);
            ASSERT_TRUE(objects[i]);
            EXPECT_EQ(objects[0]->Objects.Target, objects[i]->Objects.Target);
            auto scene = GetSceneManager()->GetSceneRT(views[i].Scene);
            ASSERT_EQ(scene->GetStaticMeshes().size(), MaterialCase ? 2u : 1u);
            for (size_t row = 0; row < scene->GetStaticMeshes().size(); ++row) {
                const auto object = scene->GetStaticMesh(scene->GetStaticMeshes()[row]);
                frame.PointBlues.push_back(Manual || row == 0 ? frame.ExpectedBlue : 179);
                auto resolved = ResolveSceneView(views[i], Width(), 96, context.GetDevice()->GetBackend());
                ASSERT_TRUE(resolved);
                const Eigen::Vector4f clip = resolved->ViewProjection * object->LocalToWorld * Eigen::Vector4f{0, 0, 0, 1};
                frame.Points.push_back({int(resolved->Scissor.X + (clip.x() / clip.w() * 0.5f + 0.5f) * resolved->Scissor.Width),
                                        int((0.5f - clip.y() / clip.w() * 0.5f) * 96)});
            }
        }
        if (MaterialCase)
            EXPECT_LE(context.GetHostWrites().GetStats().CommittedBytes - beforeBytes, 128u);
        else
            EXPECT_EQ(context.GetHostWrites().GetStats().CommittedBytes - beforeBytes, frame.ExpectStationaryObjects ? 0u : 64u);
        auto pipelineContext = MakePipelineContext(*GetRenderer(), *builder, context, GetSceneManager());
        RenderOutput output;
        const RenderOutputAttachment attachment{frame.Image.View.get(), frame.Executed ? render::TextureState::CopySource : render::TextureState::Undefined, render::TextureState::CopySource};
        if (DepthCase)
            output.Depth = attachment;
        else
            output.Colors.push_back(attachment);
        const RenderPipelineRequest request{views, std::move(output)};
        auto plan = _draw->Collect(pipelineContext, request);
        ASSERT_TRUE(builder->Compile()) << GetRenderer()->GetLastError();
        auto result = RecordRenderPipeline(*plan, pipelineContext, request);
        ASSERT_NE(result.Status, PipelineRecordStatus::RecoverableFailure);
        EXPECT_GE(renderer->GetGraphicsPipelineCache().GetSize(), 1u);
        EXPECT_LE(renderer->GetGraphicsPipelineCache().GetSize(), 2u);
        context.RegisterClosedCommandBuffers({.CmdBuffers = result.Commands});
        auto* commands = context.AllocateCommandBuffer();
        commands->CopyTextureToBuffer(frame.Readback.get(), 0, frame.Image.Tex.get(), {0, 1, 0, 1});
        context.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        if (Drop) {
            auto* window = GetWindowManager()->GetMainWindow();
            ASSERT_TRUE(target);
            auto* present = context.AllocateCommandBuffer();
            const render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{.Target = target->BackBuffer, .Before = window->GetBackBufferState(target->BackBufferIndex), .After = render::TextureState::Present};
            present->ResourceBarrier(std::span{&barrier, 1});
            context.ReturnCommandBuffers({.CmdBuffers = std::span{&present, 1}}, std::move(target));
            if (context.FrameSerial() == 1 || context.FrameSerial() == 4) {
                auto hwnd = static_cast<HWND>(window->GetNativeWindow()->GetNativeHandler());
                ::ShowWindow(hwnd, SW_HIDE);
                context.SubmitFrame();
                ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
        }
    }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        auto& frame = _frames[completion.FlightIndex];
        if (frame.Serial != completion.FrameSerial) return;
        if (!completion.OutputsExecuted) {
            ++Dropped;
        }
        frame.Executed = true;
        ScopedBufferMap map{frame.Readback.get(), {0, _pitch * 96}};
        ASSERT_TRUE(map);
        const auto* pixels = map.DataAs<uint8_t>();
        for (size_t sample = 0; sample < frame.Points.size(); ++sample) {
            const auto& point = frame.Points[sample];
            ASSERT_GE(point.x(), 0);
            ASSERT_LT(point.x(), static_cast<int>(Width()));
            ASSERT_GE(point.y(), 0);
            ASSERT_LT(point.y(), 96);
            const auto* pixel = pixels + _pitch * point.y() + point.x() * 4;
            if (DepthCase) {
                float depth = 0, oppositeDepth = 0;
                std::memcpy(&depth, pixel, 4);
                std::memcpy(&oppositeDepth, pixels + _pitch * (95 - point.y()) + point.x() * 4, 4);
                EXPECT_GT(depth, 0);
                EXPECT_LT(depth, 1);
                EXPECT_FLOAT_EQ(oppositeDepth, 1);
                continue;
            }
            // The front face has blue = 0.7; reversed winding exposes the blue = 1.0 back face.
            EXPECT_NEAR(pixel[2], frame.PointBlues[sample], 2) << "serial=" << frame.Serial << " at " << point.x() << "," << point.y();
            const auto* opposite = pixels + _pitch * (95 - point.y()) + point.x() * 4;
            EXPECT_EQ(opposite[2], 0) << "image must remain asymmetric";
        }
        ++Verified;
        if (frame.ExpectStationaryObjects) ++CameraOnlyVerified;
    }
    void OnShutdown() override {
        _draw.reset();
        _frames.clear();
        _mesh.Reset();
        _materials.clear();
        if (Drop) EXPECT_TRUE(_resized);
    }

private:
    task<void> Resize() {
        const auto result = co_await GetWindowManager()->SetSize(GetWindowManager()->GetMainWindow()->GetHandle(), 224, 112);
        EXPECT_EQ(result, WindowOperationStatus::Completed);
        _resized = result == WindowOperationStatus::Completed;
    }
    struct Frame {
        render::test::RenderTarget Image;
        unique_ptr<render::Buffer> Readback;
        vector<Eigen::Vector2i> Points;
        vector<uint8_t> PointBlues;
        Eigen::Matrix4f ExpectedView{Eigen::Matrix4f::Identity()};
        SceneId ExpectedScene;
        bool ExpectStationaryObjects{false};
        uint8_t ExpectedBlue{179};
        uint64_t Serial{0};
        bool Executed{false};
    };
    Nullable<World*> _world{nullptr};
    Nullable<Actor*> _cameraActor{nullptr};
    Nullable<CameraComponent*> _camera{nullptr};
    Nullable<StaticMeshComponent*> _object{nullptr};
    Nullable<InstancedStaticMeshComponent*> _manual{nullptr};
    array<InstanceHandle, 2> _handles{};
    unique_ptr<UnlitRenderPipeline> _draw;
    vector<Frame> _frames;
    uint64_t _pitch{0};
    uint32_t _updates{0};
    uint32_t _updateFlight{0};
    StreamingAssetRef<StaticMesh> _mesh;
    vector<StreamingAssetRef<Material>> _materials;
    TaskScope _tasks;
    bool _resized{false};
    ShapeId _oldShape;
};

void RunDraw(render::RenderBackend backend, uint32_t views, bool threaded, bool drop, bool material, bool depth, bool instanced, bool manual) {
#if !defined(RADRAY_SCENE_JIT_AVAILABLE)
    GTEST_SKIP() << "Scene drawing requires RADRAY_ENABLE_SHADER_JIT";
#endif
    test::RuntimeLogCapture logs;
    SceneDrawApp app;
    app.Views = views;
    app.Drop = drop;
    app.MaterialCase = material;
    app.DepthCase = depth;
    app.Instanced = instanced;
    app.Manual = manual;
    auto result = test::RunApplication(app, {
                                                .FlightDataCount = instanced ? 3u : 2u,
                                                .Window = drop ? std::optional<WindowOptions>{WindowOptions{.Width = 192, .Height = 96}} : std::nullopt,
                                                .Gpu = GpuOptions{
                                                    .Backend = backend,
                                                    .EnableValidation = true,
                                                    .EnableSynchronizationValidation = true,
                                                    .Multithreaded = threaded,
                                                    .EnableFrameProfiler = false,
                                                    .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
                                                    .PresentMode = render::PresentMode::FIFO,
                                                },
                                                .Render = RenderOptions{.ShaderSourceRoot = RADRAY_SCENE_EXAMPLE_DIR, .ShaderIncludePaths = {RADRAY_SHADERLIB_DIR}},
                                            });
    if (test::CanSkipRuntimeStartup(backend, result.Startup)) GTEST_SKIP() << result.Startup.Reason;
    ASSERT_EQ(result.Startup.Status, RuntimeStartupStatus::Started) << result.Startup.Reason;
    EXPECT_EQ(result.ExitCode, 0);
    EXPECT_GE(app.Verified, 8u);
    EXPECT_GE(app.CameraOnlyVerified, 3u);
    EXPECT_GE(app.ZeroViews, 2u);
    EXPECT_EQ(app.Dropped, drop ? 2u : 0u);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(SceneDraw, D3D12DepthOnly) { RunDraw(render::RenderBackend::D3D12, 3, false, false, false, true); }
TEST(SceneDraw, VulkanDepthOnly) { RunDraw(render::RenderBackend::Vulkan, 3, true, false, false, true); }
TEST(SceneDraw, D3D12MaterialGroupsOrthographic) { RunDraw(render::RenderBackend::D3D12, 3, false, false, true); }
TEST(SceneDraw, VulkanMaterialGroupsOrthographic) { RunDraw(render::RenderBackend::Vulkan, 3, true, false, true); }
TEST(SceneDraw, D3D12SevenViews) { RunDraw(render::RenderBackend::D3D12, 7, false, false); }
TEST(SceneDraw, VulkanSevenViews) { RunDraw(render::RenderBackend::Vulkan, 7, false, false); }
TEST(SceneDraw, D3D12SingleView) { RunDraw(render::RenderBackend::D3D12, 1, false, false); }
TEST(SceneDraw, VulkanSingleView) { RunDraw(render::RenderBackend::Vulkan, 1, false, false); }
TEST(SceneDraw, D3D12ThreeViewsThreaded) { RunDraw(render::RenderBackend::D3D12, 3, true, false); }
TEST(SceneDraw, VulkanThreeViewsThreaded) { RunDraw(render::RenderBackend::Vulkan, 3, true, false); }
TEST(SceneDraw, D3D12LateDropRetriesLatest) { RunDraw(render::RenderBackend::D3D12, 3, false, true); }
TEST(SceneDraw, VulkanLateDropRetriesLatest) { RunDraw(render::RenderBackend::Vulkan, 3, false, true); }
TEST(SceneDraw, D3D12InstancedMaterialThreeFlights) { RunDraw(render::RenderBackend::D3D12, 3, false, false, true, false, true); }
TEST(SceneDraw, VulkanInstancedMaterialThreeFlights) { RunDraw(render::RenderBackend::Vulkan, 3, true, false, true, false, true); }
TEST(SceneDraw, D3D12InstancedDepthThreeFlights) { RunDraw(render::RenderBackend::D3D12, 3, false, false, false, true, true); }
TEST(SceneDraw, VulkanInstancedDepthThreeFlights) { RunDraw(render::RenderBackend::Vulkan, 3, true, false, false, true, true); }
TEST(SceneDraw, D3D12ManualMaterialMatchesOrdinaryPixelsAcrossReconnect) { RunDraw(render::RenderBackend::D3D12, 3, false, false, true, false, true, true); }
TEST(SceneDraw, VulkanManualMaterialMatchesOrdinaryPixelsAcrossReconnect) { RunDraw(render::RenderBackend::Vulkan, 3, true, false, true, false, true, true); }
TEST(SceneDraw, D3D12ManualDepthMatchesOrdinaryPixelsAcrossReconnect) { RunDraw(render::RenderBackend::D3D12, 3, false, false, false, true, true, true); }
TEST(SceneDraw, VulkanManualDepthMatchesOrdinaryPixelsAcrossReconnect) { RunDraw(render::RenderBackend::Vulkan, 3, true, false, false, true, true, true); }

}  // namespace radray::test
