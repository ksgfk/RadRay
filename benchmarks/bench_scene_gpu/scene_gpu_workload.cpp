#include "scene_gpu_workload.h"
#include <radray/profiler.h>
#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif
#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/static_mesh_component.h>

namespace radray::benchmarking {

SceneGpuWorkload::SceneGpuWorkload(render::RenderBackend backend, uint32_t flights, uint32_t changes, uint32_t views, SceneGpuLoad load)
    : _timeline(flights), _load(load), _serials(flights, 0), _changes(changes), _views(views) {
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    GpuSystemDescriptor descriptor{.FlightDataCount = flights, .EnableFrameProfiler = true};
    if (backend == render::RenderBackend::Vulkan) {
        render::VulkanDeviceDescriptor vk;
        vk.Queues = std::span{&queue, 1};
        descriptor.Device = vk;
    }
    RuntimeStartupResult startup;
    _gpu = GpuSystem::TryCreate(descriptor, _timeline, startup);
    if (!_gpu) {
        _error = startup.Reason;
        return;
    }
    _renderer = make_unique<SceneManager>(flights);
    if (load == SceneGpuLoad::Parent) {
        _world = make_unique<World>();
        _world->RequestRenderConnection(_renderer.get());
        _world->FinalizeWorldGT();
        _scene = *_world->GetRenderSceneId();
        _parent = _world->SpawnActor()->AddComponent<SceneComponent>();
        for (uint32_t i = 0; i < 10000; ++i) {
            auto* component = _world->SpawnActor()->AddComponent<StaticMeshComponent>();
            component->RequestReparent(_parent);
        }
    } else if (load != SceneGpuLoad::FirstUse && load != SceneGpuLoad::Growth) {
        _scene = _renderer->CreateSceneGT();
        CreateObjects(10000);
    }
    if (load == SceneGpuLoad::FirstUse || load == SceneGpuLoad::Growth) return;
    for (uint32_t i = 0; i < flights * 2; ++i) {
        auto frame = Begin();
        Prepare(frame);
        Submit(frame);
    }
    Drain();
    UploadedBytes = UploadRanges = 0;
    GpuMilliseconds = 0;
}
void SceneGpuWorkload::CreateObjects(uint32_t count) {
    auto* writer = _renderer->GetSceneWriterGT(_scene).Get();
    _objects.reserve(10000);
    for (uint32_t i = 0; i < count; ++i) {
        auto id = writer->CreateShape();
        writer->SetStaticMesh(id, {}, Eigen::Matrix4f::Identity());
        _objects.push_back(id);
    }
}
void SceneGpuWorkload::RestartColdScene() {
    Drain();
    if (_scene.IsValid()) _renderer->DestroySceneGT(_scene);
    _scene = _renderer->CreateSceneGT();
    _objects.clear();
    CreateObjects(_load == SceneGpuLoad::Growth ? 8192 : 10000);
    if (_load == SceneGpuLoad::Growth) {
        const auto bytes = UploadedBytes, ranges = UploadRanges;
        const auto gpuTime = GpuMilliseconds;
        // Initialize each physical buffer at 8192 records before crossing its capacity.
        for (uint32_t i = 0; i < _serials.size(); ++i) {
            auto frame = Begin();
            Prepare(frame);
            Submit(frame);
        }
        Drain();
        CreateObjects(10000 - 8192);
        UploadedBytes = bytes;
        UploadRanges = ranges;
        GpuMilliseconds = gpuTime;
    }
}
SceneGpuWorkload::~SceneGpuWorkload() {
    if (!_gpu) return;
    Drain();
    if (_world) _world->ShutdownWorld();
    _renderer->BeginStoppingGT();
    _renderer->AbandonUnpublishedFramesGT();
    _timeline.CleanupCompletedFlights();
    _gpu->AbandonUnpublishedResourcesTerminalGT();
    _renderer.reset();
}
void SceneGpuWorkload::Complete(uint32_t flight) {
    if (!_serials[flight]) return;
    _gpu->CompleteFlightIfReady(flight, true);
    FlightCompletion completion;
    while (_timeline.TryReadCompletion(completion)) {
        _renderer->OnFlightCompletedGT(completion);
        _gpu->ReleaseFrameResourcesGT(completion);
        _serials[completion.FlightIndex] = 0;
    }
    GpuMilliseconds += _gpu->GetLastGpuTimeMs();
    RADRAY_PROFILE_PLOT("SceneGpu/CompletedGpuMs", double(_gpu->GetLastGpuTimeMs()));
    RADRAY_PROFILE_PLOT("SceneGpu/CompletedLatencyMs", double(_timeline.GetLastFrameLatency().count()) * 1000.0);
}
AppFrameContext SceneGpuWorkload::Begin() {
    const auto flight = BeginUpdate();
    SealScene(flight);
    auto frame = BeginRecord(flight);
    Consume(frame);
    return frame;
}
uint32_t SceneGpuWorkload::BeginUpdate() {
    const auto flight = _step % static_cast<uint32_t>(_serials.size());
    Complete(flight);
    _gpu->BeginUpdateForFlight(flight);
    _gpu->BeginFrameTiming(flight);
    return flight;
}
void SceneGpuWorkload::SealScene(uint32_t flight) {
    Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
    matrix(0, 3) = float(++_step % 65536);
    if (_world) {
        _parent->SetRelativeLocation({matrix(0, 3), 0, 0});
        _world->FinalizeWorldGT();
        _world->CollectRenderUpdates();
    } else if (_load != SceneGpuLoad::FirstUse && _load != SceneGpuLoad::Growth) {
        auto* writer = _renderer->GetSceneWriterGT(_scene).Get();
        const auto changes = _load == SceneGpuLoad::Once && _step != _serials.size() * 2 + 1 ? 0u : _changes;
        for (uint32_t i = 0; i < changes; ++i) {
            const auto index = i * (_objects.size() / _changes);
            if (_load == SceneGpuLoad::Churn) {
                writer->RemoveShape(_objects[index]);
                _objects[index] = writer->CreateShape();
                writer->SetStaticMesh(_objects[index], {}, matrix);
            } else {
                writer->SetTransform(_objects[index], matrix);
            }
        }
    }
    _renderer->SealFrameGT(flight);
    _renderer->PublishFrameGT(flight);
}
AppFrameContext SceneGpuWorkload::BeginRecord(uint32_t flight) {
    auto frame = _gpu->BeginFrameRecord(flight, {}, {}, false);
    _serials[flight] = frame.FrameSerial();
    return frame;
}
void SceneGpuWorkload::Consume(const AppFrameContext& frame) {
    _renderer->ConsumeRenderUpdates(frame.FlightIndex(), frame.FrameSerial());
}
void SceneGpuWorkload::Prepare(AppFrameContext& frame) {
    const auto before = frame.GetHostWrites().GetStats();
    for (uint32_t i = 0; i < _views; ++i) {
        const auto objects = _renderer->PrepareSceneGpuRT(_scene, frame);
        if (!objects) {
            _error = "Object buffer preparation failed";
            continue;
        }
        if (i == 0) {
            _sceneBytes[frame.FlightIndex()] = objects->Objects.Target->GetDesc().Size;
            for (const auto& parameter : objects->Parameters) _sceneBytes[frame.FlightIndex()] += parameter.Values.Target->GetDesc().Size;
        }
    }
    const auto after = frame.GetHostWrites().GetStats();
    UploadedBytes += after.CommittedBytes - before.CommittedBytes;
    UploadRanges += after.CommitCount - before.CommitCount;
}
void SceneGpuWorkload::Submit(AppFrameContext& frame) { _gpu->EndFrameRecordAndSubmit(frame.FlightIndex()); }
void SceneGpuWorkload::Drain() {
    // Consume in submission order so the last-time profiler getter counts every result.
    for (;;) {
        std::optional<uint32_t> oldest;
        for (uint32_t flight = 0; flight < _serials.size(); ++flight)
            if (_serials[flight] && (!oldest || _serials[flight] < _serials[*oldest])) oldest = flight;
        if (!oldest) break;
        Complete(*oldest);
    }
}

uint64_t SceneGpuWorkload::GetSceneAllocatedBytes() const noexcept {
    uint64_t bytes = 0;
    for (const auto size : _sceneBytes) bytes += size;
    return bytes;
}

array<uint64_t, 4> SceneGpuWorkload::GetNativeAllocatorUsage() const noexcept {
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
