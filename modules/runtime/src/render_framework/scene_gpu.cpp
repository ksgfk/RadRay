#include <radray/runtime/render_framework/scene_gpu.h>

#include <algorithm>
#include <cstring>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_system.h>

namespace radray {

SceneGpuData::SceneGpuData(render::Device* device, uint32_t flightCount) : _device(device), _flights(flightCount) {}

void SceneGpuData::ObjectSet::Add(ShapeId id) {
    if (id.Index >= Positions.size()) Positions.resize(size_t{id.Index} + 1, UINT32_MAX);
    auto& position = Positions[id.Index];
    if (position == UINT32_MAX) {
        position = static_cast<uint32_t>(Ids.size());
        Ids.push_back(id);
    } else {
        Ids[position] = id;
    }
}
void SceneGpuData::ObjectSet::Remove(ShapeId id) {
    if (id.Index >= Positions.size()) return;
    auto& position = Positions[id.Index];
    if (position == UINT32_MAX || Ids[position] != id) return;
    const auto last = Ids.back();
    Ids[position] = last;
    Positions[last.Index] = position;
    Ids.pop_back();
    position = UINT32_MAX;
}
void SceneGpuData::ObjectSet::Clear() {
    for (auto id : Ids) Positions[id.Index] = UINT32_MAX;
    Ids.clear();
}
void SceneGpuData::ApplyChanges(const SceneApplyChanges& changes) {
    for (auto& flight : _flights) {
        for (auto id : changes.Removed) flight.Pending.Remove(id);
        for (auto id : changes.Updated) flight.Pending.Add(id);
    }
}
void SceneGpuData::Complete(uint32_t flightIndex, uint64_t serial, bool executed, const RenderScene& scene) {
    auto& flight = _flights.at(flightIndex);
    if (flight.AttemptSerial != serial || flight.AttemptGeneration != flight.Generation) return;
    if (executed) {
        flight.Initialized = true;
    } else {
        for (auto id : flight.Attempt) {
            if (scene.GetStaticMesh(id)) flight.Pending.Add(id);
        }
    }
    flight.Attempt.clear();
    flight.AttemptSerial = 0;
}
std::optional<SceneGpuView> SceneGpuData::Prepare(const RenderScene& scene, AppFrameContext& frame) {
    vector<render::CommandBuffer*> commands;
    auto result = Prepare(scene, frame.FlightIndex(), frame.FrameSerial(), frame.GetCmdAllocator(), frame.GetUploader(), commands);
    if (!commands.empty()) frame.RegisterClosedCommandBuffers({.CmdBuffers = commands});
    return result;
}

std::optional<SceneGpuView> SceneGpuData::Prepare(const RenderScene& scene, PipelineContext& context, vector<render::CommandBuffer*>& outCommands) {
    return Prepare(scene, context.Frame.FlightIndex, context.Frame.FrameSerial, context.Commands, context.Resources.GetUploader(), outCommands);
}

std::optional<SceneGpuView> SceneGpuData::Prepare(const RenderScene& scene, uint32_t flightIndex, uint64_t frameSerial, ICmdAllocator& allocator, ResourceUploader& uploader, vector<render::CommandBuffer*>& outCommands) {
    RADRAY_PROFILE_SCOPE_N("SceneGpuData::Prepare");
    auto& flight = _flights.at(flightIndex);
    const auto view = [&]() -> std::optional<SceneGpuView> {
        if (!flight.PreparedValid) return std::nullopt;
        return SceneGpuView{{flight.Buffer.get(), {0, flight.Buffer->GetDesc().Size}, sizeof(SceneObjectGpuData)}, flight.Generation};
    };
    if (flight.PreparedSerial == frameSerial) return view();
    if (flight.AttemptSerial != 0) RADRAY_ABORT("Object buffer reused before completion feedback");
    flight.PreparedSerial = frameSerial;
    flight.PreparedValid = false;
    _sorted.clear();
    if (!flight.Initialized) {
        const auto ids = scene.GetStaticMeshes();
        _sorted.assign(ids.begin(), ids.end());
    } else {
        _sorted.assign(flight.Pending.Ids.begin(), flight.Pending.Ids.end());
    }
    std::sort(_sorted.begin(), _sorted.end(), [](ShapeId a, ShapeId b) { return a.Index < b.Index; });
    const uint64_t required = _sorted.empty() ? 0 : (uint64_t{_sorted.back().Index} + 1) * sizeof(SceneObjectGpuData);
    if (!flight.Buffer || flight.Buffer->GetDesc().Size < required) {
        if (scene.GetStaticMeshes().empty()) return std::nullopt;
        uint64_t capacity = flight.Buffer ? flight.Buffer->GetDesc().Size : sizeof(SceneObjectGpuData);
        while (capacity < required) capacity *= 2;
        auto buffer = _device->CreateBuffer({.Size = capacity, .Memory = render::MemoryType::Device, .Usage = render::BufferUse::Resource | render::BufferUse::CopyDestination | render::BufferUse::CopySource});
        if (!buffer) return std::nullopt;
        flight.Buffer = buffer.Release();
        ++flight.Generation;
        flight.Initialized = false;
        const auto ids = scene.GetStaticMeshes();
        _sorted.assign(ids.begin(), ids.end());
        std::sort(_sorted.begin(), _sorted.end(), [](ShapeId a, ShapeId b) { return a.Index < b.Index; });
    }
    RADRAY_PROFILE_PLOT("SceneGpu/UploadObjects", static_cast<int64_t>(_sorted.size()));
    if (_sorted.empty()) RADRAY_PROFILE_PLOT("SceneGpu/UploadRanges", int64_t{0});
    if (!_sorted.empty()) {
        _packed.resize(_sorted.size());
        _uploads.clear();
        auto state = flight.Initialized ? render::BufferState::ShaderRead : render::BufferState::Common;
        for (size_t begin = 0; begin < _sorted.size();) {
            size_t end = begin + 1;
            while (end < _sorted.size() && _sorted[end].Index == _sorted[end - 1].Index + 1) ++end;
            for (size_t i = begin; i < end; ++i) {
                const auto object = scene.GetStaticMesh(_sorted[i]);
                if (!object) RADRAY_ABORT("Stale object in upload set");
                std::memcpy(_packed[i].LocalToWorld.data(), object->LocalToWorld.data(), sizeof(SceneObjectGpuData));
            }
            _uploads.push_back({.SrcData = std::as_bytes(std::span{_packed}.subspan(begin, end - begin)), .DstBuffer = flight.Buffer.get(), .DstOffset = uint64_t{_sorted[begin].Index} * sizeof(SceneObjectGpuData), .Before = state, .After = render::BufferState::ShaderRead});
            begin = end;
        }
        auto* commands = allocator.Allocate();
        RADRAY_PROFILE_PLOT("SceneGpu/UploadRanges", static_cast<int64_t>(_uploads.size()));
        const bool recorded = uploader.TryUploadBufferRanges(commands, _uploads);
        allocator.Return(std::span{&commands, 1});
        outCommands.push_back(commands);
        if (!recorded) return std::nullopt;
        flight.Attempt.assign(_sorted.begin(), _sorted.end());
        flight.AttemptSerial = frameSerial;
        flight.AttemptGeneration = flight.Generation;
        flight.Pending.Clear();
    }
    flight.PreparedValid = flight.Initialized || !flight.Attempt.empty();
    return view();
}

}  // namespace radray
