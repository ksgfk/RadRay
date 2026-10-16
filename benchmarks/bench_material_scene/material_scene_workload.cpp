#include "material_scene_workload.h"
#include "scene_draw.h"
#include <radray/profiler.h>
#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif
#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif
#include <radray/runtime/game_framework/actor.h>
#include <cstring>
#include <fmt/format.h>

namespace radray::benchmarking {
MaterialSceneWorkload::MaterialSceneWorkload(render::RenderBackend backend, uint32_t count, uint32_t materials, uint32_t views, uint32_t percent, uint32_t churn, uint32_t mode, bool uniqueAssets, uint32_t flights)
    : _timeline(flights), _uniqueAssets(uniqueAssets), _serials(flights), _colors(flights), _colorViews(flights), _initialized(flights), _count(count), _changes(count * percent / 100), _churn(churn), _mode(mode) {
    const uint64_t requiredDescriptors = uint64_t{materials} + uint64_t{flights} * views * (mode == 0 ? 4u : mode == 3 ? 8u
                                                                                                                       : 6u);
    if (backend == render::RenderBackend::D3D12 && requiredDescriptors > (1u << 16)) {
        _error = fmt::format("D3D12 pressure requires {} descriptors; renderer heap capacity is 65536", requiredDescriptors);
        return;
    }
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    GpuSystemDescriptor descriptor{.FlightDataCount = flights, .EnableFrameProfiler = true};
    if (backend == render::RenderBackend::Vulkan) {
        render::VulkanDeviceDescriptor vk;
        vk.Queues = std::span{&queue, 1};
        descriptor.Device = vk;
    }
    RuntimeStartupResult startup;
    descriptor.RhiValidation.IsEnabled = false;
    _gpu = GpuSystem::TryCreate(descriptor, _timeline, startup);
    if (!_gpu) {
        _error = startup.Reason;
        return;
    }
    auto* device = _gpu->GetDevice();
    _renderer = make_unique<RenderSystem>(device, RADRAY_BENCH_SHADER_DIR, vector<std::filesystem::path>{RADRAY_SHADERLIB_DIR});
    if (!_renderer->OnInitialize()) {
        _error = "RenderSystem initialization failed";
        return;
    }
    _framework = make_unique<Renderer>(*_gpu, *_renderer);
    auto cube = example::CreateCube(*_gpu);
    if (!cube) {
        _error = "Cube upload failed";
        return;
    }
    _mesh = _assets.AddReady<StaticMesh>({1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, std::move(cube));
    MaterialTemplateDescriptor pass;
    pass.Name = "Unlit";
    pass.Program.SourceName = mode == 0 ? "pressure.hlsl" : mode == 3 ? "pressure_tint_instanced.hlsl"
                                                                      : "pressure_instanced.hlsl";
    pass.Inputs = {{"Objects", MaterialInputSource::SceneObjects}, {"ViewData", MaterialInputSource::ViewConstants}, {"MaterialData", MaterialInputSource::MaterialConstants}};
    pass.ObjectIndexPushConstant = "DrawData";
    if (mode) {
        pass.ObjectIndexPushConstant.clear();
        pass.PushConstant = "DrawData";
        pass.SceneIndexField = "VisibleBase";
        pass.InstanceAddressing = MaterialInstanceAddressing::VisibleSlots;
        pass.AllowAutomaticInstancing = mode == 1;
        pass.Inputs.push_back({"VisibleInstanceSlots", MaterialInputSource::VisibleInstanceSlots});
    }
    const auto program = _renderer->GetOrCreateShaderProgram(pass.Program);
    if (!program) {
        _error = "Pressure shader compilation failed";
        return;
    }
    pass.RegisteredProgram = program->GetId();
    pass.Program = {};
    if (mode == 3) {
        const auto layout = ReflectInstanceParameterLayout(program->GetArtifact().Generic(), "MaterialData", "InstanceTint");
        if (!layout) {
            _error = "Instance tint schema reflection failed";
            return;
        }
        _parameterLayout = *layout;
        pass.ParameterLayouts = {*layout};
        pass.Inputs.push_back({"InstanceTint", MaterialInputSource::InstanceParameters});
    }
    const auto definition = std::move(pass);
    for (uint32_t i = 0; i < materials; ++i) {
        const array<float, 4> tint{0.5f + float(i % 7) / 14, 0.8f, 0.6f, 1};
        const auto bytes = std::as_bytes(std::span{tint});
        auto asset = make_unique<Material>(vector{definition}, vector<vector<byte>>{vector<byte>(bytes.begin(), bytes.end())});
        _materials.push_back(_assets.AddReady<Material>({i + 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, std::move(asset)));
    }
    _scenes = make_unique<SceneManager>(flights);
    _scenes->AttachRenderer(*_framework);
    _world = make_unique<World>();
    _world->RequestRenderConnection(_scenes.get());
    _world->FinalizeWorldGT();
    if (mode < 2)
        for (uint32_t i = 0; i < count; ++i) _objects.push_back(AddObject(i));
    else {
        if (materials != 1) {
            _error = "The manual workload uses one shared material per collection";
            return;
        }
        _instanced = _world->SpawnActor()->AddComponent<InstancedStaticMeshComponent>();
        _instanced->SetStaticMesh(_mesh);
        _instanced->SetMaterials({_materials[0]});
        vector<InstanceInit> input(count);
        _instances.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            input[i].LocalToComponent.diagonal().head<3>().setConstant(.025f);
            input[i].LocalToComponent.block<3, 1>(0, 3) = Eigen::Vector3f{float(i % 100) * .1f - 5, float(i / 100 % 100) * .1f - 5, 4};
        }
        if (!_instanced->AddInstances(input, _instances)) {
            _error = "Manual collection creation failed";
            return;
        }
        if (mode == 3) {
            vector<byte> colors(size_t{count} * 16);
            const array<float, 4> white{1, 1, 1, 1};
            for (size_t i = 0; i < count; ++i) std::memcpy(colors.data() + i * 16, white.data(), 16);
            if (!_instanced->SetInstanceParameters(_instances, _parameterLayout, colors)) {
                _error = "Manual tint initialization failed";
                return;
            }
        }
    }
    for (uint32_t i = 0; i < views; ++i) {
        SceneViewRequest view;
        view.Scene = *_world->GetRenderSceneId();
        view.Projection = SceneProjection::Orthographic;
        view.OrthographicHeight = 20;
        view.View(0, 3) = float(i) * 0.1f;
        _views.push_back(view);
    }
    for (uint32_t i = 0; i < flights; ++i) {
        auto color = device->CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = 256, .Height = 256, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Usage = render::TextureUse::RenderTarget});
        if (!color) {
            _error = "Target allocation failed";
            return;
        }
        _colors[i] = color.Release();
        auto view = device->CreateTextureView({_colors[i].get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget});
        if (!view) {
            _error = "Target view allocation failed";
            return;
        }
        _colorViews[i] = view.Release();
    }
    for (uint32_t i = 0; i < 8 && _error.empty(); ++i) {
        const auto flight = BeginUpdate();
        Sync(flight);
        auto frame = BeginRecord(flight);
        PrepareAndRecord(frame);
        Submit(frame);
    }
    Drain();
    Frames = Draws = UploadBytes = 0;
    GpuMs = 0;
    RADRAY_PROFILE_MESSAGE("MaterialScene warmup complete; single-thread");
}
StaticMeshComponent* MaterialSceneWorkload::AddObject(uint32_t index) {
    auto* component = _world->SpawnActor()->AddComponent<StaticMeshComponent>();
    auto mesh = _mesh;
    auto material = _materials[index % _materials.size()];
    if (_uniqueAssets && _step) {
        const auto& source = _mesh->GetMeshResource();
        auto geometry = AllocateRuntimeMesh(*_gpu->GetDevice(), source);
        if (!geometry)
            _error = "Unique churn mesh allocation failed";
        else {
            mesh = _assets.AddReady<StaticMesh>({_nextAsset++, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
                                                make_unique<StaticMesh>(source, _mesh->GetSections(), _mesh->GetBoundsMin(), _mesh->GetBoundsMax(), std::move(*geometry)));
            _uploadOwners.push_back(mesh);
            _uploadMeshes.push_back(mesh.Get().Get());
        }
        const auto& data = material->GetRenderData();
        material = _assets.AddReady<Material>({_nextAsset++, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<Material>(data.Passes, data.Constants));
    }
    component->SetStaticMesh(mesh);
    component->SetMaterials({material});
    component->SetRelativeLocation({float(index % 100) * 0.1f - 5, float(index / 100 % 100) * 0.1f - 5, 4});
    component->SetRelativeScale({0.025f, 0.025f, 0.025f});
    return component;
}
void MaterialSceneWorkload::Complete(uint32_t flight) {
    if (!_serials[flight]) return;
    _gpu->CompleteFlightIfReady(flight, true);
    FlightCompletion completion;
    while (_timeline.TryReadCompletion(completion)) {
        _gpu->ReleaseFrameResourcesGT(completion);
        _framework->ProcessCompletions();
        _scenes->OnFlightCompletedGT(completion);
        _serials[completion.FlightIndex] = 0;
    }
    GpuMs += _gpu->GetLastGpuTimeMs();
    RADRAY_PROFILE_PLOT("MaterialScene/CompletedGpuMs", double(_gpu->GetLastGpuTimeMs()));
    RADRAY_PROFILE_PLOT("MaterialScene/CompletedLatencyMs", double(_timeline.GetLastFrameLatency().count()) * 1000.0);
}
uint32_t MaterialSceneWorkload::BeginUpdate() {
    const auto flight = _step++ % _timeline.GetFlightDataCount();
    Complete(flight);
    _gpu->BeginUpdateForFlight(flight);
    _gpu->BeginFrameTiming(flight);
    _uploadMeshes.clear();
    return flight;
}
void MaterialSceneWorkload::Sync(uint32_t flight) {
    RADRAY_PROFILE_SCOPE_N("MaterialScene::Sync");
    {
        RADRAY_PROFILE_SCOPE_N("MaterialScene::Mutation");
        vector<StaticMeshComponent*> temporary;
        _instanceUpdates.clear();
        _parameterHandles.clear();
        _parameterValues.clear();
        if (_churn) temporary.reserve(_changes);
        for (uint32_t i = 0; i < _changes; ++i) {
            const auto index = size_t{i} * _count / _changes;
            if (!_churn && _mode >= 2) {
                if (_mode == 3) {
                    _parameterHandles.push_back(_instances[index]);
                    const array<float, 4> tint{_step % 2 ? .9f : 1.0f, 1, 1, 1};
                    const auto bytes = std::as_bytes(std::span{tint});
                    _parameterValues.insert(_parameterValues.end(), bytes.begin(), bytes.end());
                } else {
                    Eigen::Matrix4f local = Eigen::Matrix4f::Identity();
                    local.diagonal().head<3>().setConstant(.025f);
                    local.block<3, 1>(0, 3) = Eigen::Vector3f{float(index % 100) * .1f - 5, float(index / 100 % 100) * .1f - 5, _step % 2 ? 4.01f : 4.0f};
                    _instanceUpdates.push_back({_instances[index], local});
                }
            } else if (!_churn) {
                _objects[index]->SetRelativeLocation({float(index % 100) * 0.1f - 5, float(index / 100 % 100) * 0.1f - 5, _step % 2 ? 4.01f : 4.0f});
            } else {
                temporary.push_back(AddObject(static_cast<uint32_t>(index)));
            }
        }
        if (_instanced && _mode == 2 && !_instanced->UpdateInstances(_instanceUpdates)) _error = "Manual update failed";
        if (_instanced && _mode == 3 && !_parameterHandles.empty() && !_instanced->SetInstanceParameters(_parameterHandles, _parameterLayout, _parameterValues)) _error = "Manual tint update failed";
        if (_churn >= 2) {
            _world->FinalizeWorldGT();
            _world->CollectRenderUpdates();
        }
        if (_churn == 3) {
            _scenes->SealFrameGT(flight);
            _scenes->PublishFrameGT(flight);
        }
        for (auto* component : temporary) _world->DestroyActor(component->GetOwner().Get());
    }
    _world->FinalizeWorldGT();
    _world->CollectRenderUpdates();
    if (_churn != 3 || !_changes) {
        _scenes->SealFrameGT(flight);
        _scenes->PublishFrameGT(flight);
    }
    if (!_uploadOwners.empty()) _gpu->RetainForFrameGT(flight, std::exchange(_uploadOwners, {}));
    if (_uniqueAssets) _assets.Pump();
}
AppFrameContext MaterialSceneWorkload::BeginRecord(uint32_t flight) {
    auto frame = _gpu->BeginFrameRecord(flight, {}, {}, false);
    _serials[flight] = frame.FrameSerial();
    _scenes->ConsumeRenderUpdates(flight, frame.FrameSerial());
    return frame;
}
void MaterialSceneWorkload::PrepareAndRecord(AppFrameContext& frame) {
    RADRAY_PROFILE_SCOPE_N("MaterialScene::PrepareAndRecord");
    const auto flight = frame.FlightIndex();
    auto builder = _framework->BeginFrame(frame);
    if (!builder) { _error = _framework->GetLastError(); return; }
    auto context = MakePipelineContext(*_framework, *builder, frame, _scenes.get());
    if (!_uploadMeshes.empty()) {
        auto* command = context.Commands.Allocate();
        for (const auto* mesh : _uploadMeshes)
            if (!TryUploadRuntimeMesh(context.Resources.GetUploader(), command, mesh->GetMeshResource(), mesh->GetRenderMesh())) {
                _error = "Unique churn mesh upload failed";
                break;
            }
        context.Commands.Return(std::span{&command, 1});
        frame.RegisterRequired(std::span{&command, 1});
        if (!_error.empty()) return;
    }
    const auto before = frame.GetHostWrites().GetStats().CommittedBytes;
    const auto objects = _framework->GetScenes().Prepare(_scenes->GetRendererSceneKey(*_world->GetRenderSceneId()), *_scenes->GetSceneRT(*_world->GetRenderSceneId()), context);
    if (!objects) {
        _error = "Scene GPU prepare failed";
        return;
    }

    auto* view = _colorViews[flight].get();
    _sceneBytes[frame.FlightIndex()] = objects->Objects.Target->GetDesc().Size;
    for (const auto& parameter : objects->Parameters) _sceneBytes[frame.FlightIndex()] += parameter.Values.Target->GetDesc().Size;
    const RenderOutput output{{{view, _initialized[flight] ? render::TextureState::RenderTarget : render::TextureState::Undefined, render::TextureState::RenderTarget}}, {}};
    const auto info = ValidateRenderOutput(output, *frame.GetDevice());
    vector<PreparedSceneDraw> prepared;
    for (const auto& request : _views) {
        auto item = _draw.Prepare(context, request, *objects, *info, "Unlit", std::nullopt, false);
        if (!item) {
            _error = "Scene draw prepare failed";
            return;
        }
        Draws += item->Draws.size();
        prepared.push_back(std::move(*item));
    }
    if (!builder->Compile()) { _error = _framework->GetLastError(); return; }
    auto* commands = context.Commands.Allocate();
    const render::ColorClearValue clear{{0, 0, 0, 1}};
    commands->PushDebugGroup("MaterialSceneGPU");
    auto state = InitialRenderOutputState(output);
    TransitionRenderOutput(commands, output, state, false);
    auto encoder = commands->BeginRenderPass({array{render::RenderPassColorAttachment{view, render::LoadAction::Clear, render::StoreAction::Store, clear}}, {}, 256, 256, 1, {}, "MaterialSceneGPU"});
    if (!encoder) {
        commands->PopDebugGroup();
        context.Commands.Discard(std::span{&commands, 1});
        _error = "Render encoder failed";
        return;
    }
    for (const auto& item : prepared) item.Record(*builder, encoder.Get());
    commands->EndRenderPass(encoder.Release());
    commands->PopDebugGroup();
    context.Commands.Return(std::span{&commands, 1});
    frame.RegisterClosedCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
    _initialized[flight] = true;
    UploadBytes += frame.GetHostWrites().GetStats().CommittedBytes - before;
    ++Frames;
    RADRAY_PROFILE_FRAME();
}
void MaterialSceneWorkload::Submit(AppFrameContext& frame) { _gpu->EndFrameRecordAndSubmit(frame.FlightIndex()); }
void MaterialSceneWorkload::Drain() {
    // Consume in submission order so the last-time profiler getter counts every result.
    for (;;) {
        std::optional<uint32_t> oldest;
        for (uint32_t flight = 0; flight < _serials.size(); ++flight)
            if (_serials[flight] && (!oldest || _serials[flight] < _serials[*oldest])) oldest = flight;
        if (!oldest) break;
        Complete(*oldest);
    }
}
MaterialSceneWorkload::~MaterialSceneWorkload() {
    if (!_gpu) return;
    Drain();
    if (_framework) _framework->Shutdown();
    if (_world) _world->ShutdownWorld();
    if (_scenes) {
        _scenes->BeginStoppingGT();
        _scenes->AbandonUnpublishedFramesGT();
    }
    _timeline.CleanupCompletedFlights();
    _gpu->AbandonUnpublishedResourcesTerminalGT();
    _scenes.reset();
    _objects.clear();
    _world.reset();
    _mesh.Reset();
    _materials.clear();
    _assets.Pump();
    if (_renderer) _renderer->OnShutdown();
}
uint64_t MaterialSceneWorkload::GetSceneAllocatedBytes() const noexcept {
    uint64_t bytes = 0;
    for (const auto size : _sceneBytes) bytes += size;
    return bytes;
}

array<uint64_t, 4> MaterialSceneWorkload::GetNativeAllocatorUsage() const noexcept {
    if (!_gpu) return {};
    auto* device = _gpu->GetDevice();
#if defined(RADRAY_ENABLE_D3D12)
    if (device->GetBackend() == render::RenderBackend::D3D12) {
        D3D12MA::TotalStatistics stats{};
        static_cast<render::d3d12::DeviceD3D12*>(device)->_mainAlloc->CalculateStatistics(&stats);
        const auto& total = stats.Total.Stats;
        return {total.BlockBytes, total.AllocationBytes, total.BlockCount, total.AllocationCount};
    }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (device->GetBackend() == render::RenderBackend::Vulkan) {
        VmaTotalStatistics stats{};
        vmaCalculateStatistics(static_cast<render::vulkan::DeviceVulkan*>(device)->_vma->_vma, &stats);
        const auto& total = stats.total.statistics;
        return {total.blockBytes, total.allocationBytes, total.blockCount, total.allocationCount};
    }
#endif
    return {};
}

}  // namespace radray::benchmarking
