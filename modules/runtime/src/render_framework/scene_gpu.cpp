#include <radray/runtime/render_framework/scene_gpu.h>

#include <algorithm>
#include <cstring>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/renderer.h>

namespace radray {

SceneGpuData::SceneGpuData(Renderer& renderer, uint32_t flightCount) : _renderer(renderer), _flights(flightCount) {}
SceneGpuData::~SceneGpuData() noexcept {
    for (const auto& flight : _flights) {
        if (flight.Buffer.IsValid()) _renderer.RetireResource(flight.Buffer);
        for (const auto& [name, parameters] : flight.Parameters)
            if (parameters.Buffer.IsValid()) _renderer.RetireResource(parameters.Buffer);
    }
}

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
        for (auto& [name, parameters] : flight.Parameters)
            for (auto id : changes.Removed) parameters.Pending.Remove(id);
        for (const auto& change : changes.Parameters) flight.Parameters[change.Stream].Pending.Add(change.Id);
    }
}
std::optional<SceneGpuView> SceneGpuData::Prepare(const RenderScene& scene, AppFrameContext& frame, FrameBuilder& builder) {
    if (!builder.IsFor(frame)) return std::nullopt;
    return Prepare(scene, frame.FlightIndex(), frame.FrameSerial(), frame.GetCmdAllocator(), frame.GetUploader(), frame, builder);
}

std::optional<SceneGpuView> SceneGpuData::Prepare(const RenderScene& scene, PipelineContext& context) {
    if (!context.RequiredWork) {
        RADRAY_ERR_LOG("Scene preparation requires a required-work registrar");
        return std::nullopt;
    }
    return Prepare(scene, context.Frame.FlightIndex, context.Frame.FrameSerial, context.Commands, context.Resources.GetUploader(), *context.RequiredWork, context.Builder);
}

std::optional<SceneGpuView> SceneGpuData::Prepare(const RenderScene& scene, uint32_t flightIndex, uint64_t frameSerial, IGpuCmdAllocator& allocator, ResourceUploader& uploader, IRequiredGpuWork& requiredWork, FrameBuilder& builder) {
    RADRAY_PROFILE_SCOPE_N("SceneGpuData::Prepare");
    if (!builder.IsFor(_renderer, flightIndex, frameSerial)) return std::nullopt;
    auto& flight = _flights.at(flightIndex);
    const auto view = [&]() -> std::optional<SceneGpuView> {
        if (!flight.PreparedValid) return std::nullopt;
        return SceneGpuView{{_renderer.GetBuffer(flight.Buffer).Get(), {0, _renderer.GetBuffer(flight.Buffer)->GetDesc().Size}, sizeof(SceneObjectGpuData)}, flight.Generation, flight.ParameterViews, flight.ObjectsResource, flight.ParameterResources};
    };
    if (flight.PreparedSerial == frameSerial) return view();
    flight.PreparedSerial = frameSerial;
    flight.PreparedValid = false;
    flight.ParameterViews.clear();
    flight.ParameterResources.clear();
    _sorted.clear();
    if (!flight.Initialized) {
        const auto ids = scene.GetStaticMeshes();
        _sorted.assign(ids.begin(), ids.end());
    } else {
        _sorted.assign(flight.Pending.Ids.begin(), flight.Pending.Ids.end());
    }
    std::sort(_sorted.begin(), _sorted.end(), [](ShapeId a, ShapeId b) { return a.Index < b.Index; });
    const uint64_t required = _sorted.empty() ? 0 : (uint64_t{_sorted.back().Index} + 1) * sizeof(SceneObjectGpuData);
    if (!flight.Buffer.IsValid() || _renderer.GetBuffer(flight.Buffer)->GetDesc().Size < required) {
        if (scene.GetStaticMeshes().empty()) return std::nullopt;
        uint64_t capacity = flight.Buffer.IsValid() ? _renderer.GetBuffer(flight.Buffer)->GetDesc().Size : sizeof(SceneObjectGpuData);
        while (capacity < required) capacity *= 2;
        auto buffer = _renderer.CreateBuffer({.Size = capacity, .Memory = render::MemoryType::Device, .Usage = render::BufferUse::Resource | render::BufferUse::CopyDestination | render::BufferUse::CopySource});
        if (!buffer) return std::nullopt;
        if (flight.Buffer.IsValid()) _renderer.RetireResource(flight.Buffer);
        flight.Buffer = *buffer;
        ++flight.Generation;
        flight.Initialized = false;
        const auto ids = scene.GetStaticMeshes();
        _sorted.assign(ids.begin(), ids.end());
        std::sort(_sorted.begin(), _sorted.end(), [](ShapeId a, ShapeId b) { return a.Index < b.Index; });
    }
    if (!builder.UseResource(flight.Buffer)) return std::nullopt;
    const auto objectsResource = _renderer.CreateBufferRange(flight.Buffer, {0, render::BufferRange::All()}, sizeof(SceneObjectGpuData));
    if (!objectsResource || !builder.UseResource(*objectsResource)) return std::nullopt;
    flight.ObjectsResource = *objectsResource;
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
                const auto transform = scene.GetStaticMeshTransform(_sorted[i]);
                if (!transform) RADRAY_ABORT("Stale object in upload set");
                std::memcpy(_packed[i].LocalToWorld.data(), transform->data(), sizeof(SceneObjectGpuData));
            }
            _uploads.push_back({.SrcData = std::as_bytes(std::span{_packed}.subspan(begin, end - begin)), .DstBuffer = _renderer.GetBuffer(flight.Buffer).Get(), .DstOffset = uint64_t{_sorted[begin].Index} * sizeof(SceneObjectGpuData), .Before = state, .After = render::BufferState::ShaderRead});
            begin = end;
        }
        auto* commands = allocator.Allocate();
        RADRAY_PROFILE_PLOT("SceneGpu/UploadRanges", static_cast<int64_t>(_uploads.size()));
        const bool recorded = uploader.TryUploadBufferRanges(commands, _uploads);
        allocator.Return(std::span{&commands, 1});
        requiredWork.RegisterRequired(std::span{&commands, 1});
        if (!recorded) return std::nullopt;
        flight.Initialized = true;
        flight.Pending.Clear();
    }
    for (const auto& [name, stream] : scene.GetParameterStreams()) {
        auto& parameters = flight.Parameters[name];
        const uint64_t bytes = uint64_t{stream.Rows.size()} * stream.Layout.Stride;
        if (bytes > 64 * 1024 * 1024) {
            RADRAY_ERR_LOG("Scene parameter stream {} exceeds the 64 MiB global-slot buffer limit", name);
            return std::nullopt;
        }
        if (!parameters.Buffer.IsValid() || _renderer.GetBuffer(parameters.Buffer)->GetDesc().Size < bytes) {
            uint64_t capacity = parameters.Buffer.IsValid() ? _renderer.GetBuffer(parameters.Buffer)->GetDesc().Size : stream.Layout.Stride;
            while (capacity < bytes) capacity *= 2;
            auto buffer = _renderer.CreateBuffer({capacity, render::MemoryType::Device, render::BufferUse::Resource | render::BufferUse::CopySource | render::BufferUse::CopyDestination});
            if (!buffer) return std::nullopt;
            if (parameters.Buffer.IsValid()) _renderer.RetireResource(parameters.Buffer);
            parameters.Buffer = *buffer;
            ++parameters.Generation;
            parameters.Initialized = false;
        }
        if (!builder.UseResource(parameters.Buffer)) return std::nullopt;
        const auto parameterResource = _renderer.CreateBufferRange(parameters.Buffer, {0, render::BufferRange::All()}, stream.Layout.Stride);
        if (!parameterResource || !builder.UseResource(*parameterResource)) return std::nullopt;
        flight.ParameterResources.push_back({stream.Layout, *parameterResource});
        auto& dirty = parameters.Sorted;
        dirty.clear();
        if (!parameters.Initialized) {
            for (const auto& row : stream.Rows)
                if (row) dirty.push_back(row->Id);
        } else
            dirty = parameters.Pending.Ids;
        std::sort(dirty.begin(), dirty.end(), [](ShapeId a, ShapeId b) { return a.Index < b.Index; });
        auto& uploads = parameters.Uploads;
        uploads.clear();
        parameters.Packed.resize(dirty.size() * stream.Layout.Stride);
        size_t packed = 0;
        uint32_t previous = UINT32_MAX;
        for (auto id : dirty) {
            if (id.Index >= stream.Rows.size() || !stream.Rows[id.Index] || stream.Rows[id.Index]->Id != id) continue;
            const auto values = stream.GetValues(id.Index);
            std::memcpy(parameters.Packed.data() + packed, values.data(), values.size());
            if (!uploads.empty() && previous != UINT32_MAX && previous + 1 == id.Index)
                uploads.back().SrcData = {uploads.back().SrcData.data(), uploads.back().SrcData.size() + values.size()};
            else
                uploads.push_back({std::span<const byte>{parameters.Packed.data() + packed, values.size()}, _renderer.GetBuffer(parameters.Buffer).Get(), uint64_t{id.Index} * stream.Layout.Stride,
                                   parameters.Initialized ? render::BufferState::ShaderRead : render::BufferState::Common, render::BufferState::ShaderRead});
            previous = id.Index;
            packed += values.size();
        }
        if (!uploads.empty()) {
            auto* command = allocator.Allocate();
            const bool recorded = uploader.TryUploadBufferRanges(command, uploads);
            allocator.Return(std::span{&command, 1});
            requiredWork.RegisterRequired(std::span{&command, 1});
            if (!recorded) return std::nullopt;
            parameters.Initialized = true;
            parameters.Pending.Clear();
        }
        flight.ParameterViews.push_back({&stream.Layout, {_renderer.GetBuffer(parameters.Buffer).Get(), {0, _renderer.GetBuffer(parameters.Buffer)->GetDesc().Size}, stream.Layout.Stride}});
    }
    flight.PreparedValid = flight.Initialized;
    return view();
}

}  // namespace radray
