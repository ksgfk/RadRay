#include <radray/runtime/renderer.h>

#include <limits>
#include <algorithm>

#include <radray/logger.h>
#include <radray/environment.h>

namespace radray {
namespace {
ResourceHandle Import(GraphBuilder& g, const BindingResource& r) {
    if (auto existing = g.Find(r.buffer ? static_cast<render::Resource*>(r.buffer.Get()) : static_cast<render::Resource*>(r.texture.Get()))) return *existing;
    return r.buffer ? g.ImportBuffer("Mesh buffer", r.buffer, r.import) : g.ImportTexture("Mesh texture", r.texture, r.import);
}
BindingResource BufferBinding(render::Buffer* buffer, Access access, render::BufferStates state, shared_ptr<void> owner = {}) {
    const auto s = ResourceState::Buffer(state);
    return {buffer, nullptr, access, {}, ImportOptions{s, s, true, false, false, std::move(owner)}};
}
}  // namespace
vector<ResourceAccess> PreparedMeshPass::Declare(GraphBuilder& graph) const {
    vector<ResourceAccess> result;
    const auto add = [&](const BindingResource& r) { if(r.buffer || r.texture) result.push_back({Import(graph,r),r.access,r.stages}); };
    for (const auto& [id, r] : pages) add(r);
    for (const auto& [id, r] : indices) add(r);
    add(instances);
    if (!direct) add(indirect);
    for (const auto& [id, binding] : bindings)
        for (const auto& r : binding.resources) add(r);
    return result;
}
bool PreparedMeshPass::Draw(PassContext& context) const {
    auto pass = context.Raster();
    if (!pass) return false;
    for (size_t i = 0; i < draws.packets.size();) {
        const auto& p = draws.packets[i];
        const auto& b = bindings.at(p.material);
        if (!b.pipeline || !b.parameters) {
            RADRAY_ERR_LOG("Mesh binding lacks a pipeline or parameters");
            return false;
        }
        pass->BindGraphicsPipelineState(b.pipeline.Get());
        pass->BindShaderParameterSet(0, b.parameters.Get());
        vector<render::VertexBufferBinding> streams;
        if (b.vertexSlot) {
            auto* buffer = pages.at(p.page).buffer.Get();
            streams.push_back({*b.vertexSlot, {buffer, 0, buffer->GetDesc().Size}});
        }
        if (b.instanceSlot) streams.push_back({*b.instanceSlot, {instances.buffer.Get(), 0, instances.buffer->GetDesc().Size}});
        if (!streams.empty()) pass->BindVertexBuffers(streams);
        if (p.indexBuffer) pass->BindIndexBuffer({indices.at(p.indexBuffer).buffer.Get(), 0, 4});
        size_t end = i + 1;
        while (end < draws.packets.size()) {
            const auto& n = draws.packets[end];
            if (n.program != p.program || n.material != p.material || n.page != p.page || n.indexBuffer != p.indexBuffer || n.state != p.state) break;
            ++end;
        }
        if (direct)
            for (size_t j = i; j < end; ++j) {
                const auto& draw = draws.packets[j];
                if (draw.indexBuffer)
                    pass->DrawIndexed(draw.count, draw.instances, draw.first, draw.baseVertex, draw.firstInstance);
                else
                    pass->Draw(draw.count, draw.instances, draw.first, draw.firstInstance);
            }
        else {
            if (p.indexBuffer)
                pass->DrawIndexedIndirect(indirect.buffer.Get(), p.indirectOffset, static_cast<uint32_t>(end - i));
            else
                pass->DrawIndirect(indirect.buffer.Get(), p.indirectOffset, static_cast<uint32_t>(end - i));
        }
        i = end;
    }
    return true;
}
FrameContext::FrameContext(
    render::Device& device,
    uint32_t slot,
    unique_ptr<render::CommandAllocator> allocator,
    unique_ptr<render::CommandBuffer> command,
    unique_ptr<render::QueryPool> timestamps,
    RenderBuffer timing)
    : _device(device), _slot(slot), _allocator(std::move(allocator)), _command(std::move(command)), _timestamps(std::move(timestamps)), _timing(std::move(timing)) {}

void FrameContext::KeepAlive(shared_ptr<void> owner) {
    _keepAlive.push_back(std::move(owner));
}

Nullable<render::Buffer*> FrameContext::Upload(string name, std::span<const byte> data, render::BufferUses usage) {
    if (data.empty()) return nullptr;
    auto& upload = _uploads[std::move(name)];
    if (upload.size < data.size() || upload.usage != usage) {
        auto replacement = CreateBuffer(_device, data.size(), render::MemoryType::Upload, usage).Release();
        if (!replacement) {
            RADRAY_ERR_LOG("Cannot allocate frame upload buffer");
            return nullptr;
        }
        upload = {std::move(replacement), data.size(), usage};
    }
    if (!WriteBuffer(*upload.buffer, data.data(), data.size())) {
        RADRAY_ERR_LOG("Cannot write frame upload buffer");
        return nullptr;
    }
    return upload.buffer.get();
}
Renderer::Renderer(DeviceContext& d, unique_ptr<IRenderPipeline> p, shared_ptr<render::Fence> fence, array<unique_ptr<FrameContext>, 2> frames)
    : _device(d), _pipeline(std::move(p)), _completion(std::move(fence)), _frames(std::move(frames)), _pool(d.GetDevice()), _geometry(d) {}

Nullable<unique_ptr<Renderer>> Renderer::Create(DeviceContext& device, unique_ptr<IRenderPipeline> pipeline) {
    if (!pipeline) {
        RADRAY_ERR_LOG("Renderer requires a pipeline");
        return nullptr;
    }
    auto& d = device.GetDevice();
    auto completion = d.CreateFence().Release();
    if (!completion) {
        RADRAY_ERR_LOG("Cannot create renderer completion fence");
        return nullptr;
    }
    array<unique_ptr<FrameContext>, 2> frames;
    for (uint32_t i = 0; i < frames.size(); ++i) {
        auto allocator = d.CreateCommandAllocator(&device.GetQueue()).Release();
        if (!allocator) {
            RADRAY_ERR_LOG("Cannot create frame allocator");
            return nullptr;
        }
        auto command = d.CreateCommandBuffer(allocator.get()).Release();
        if (!command) {
            RADRAY_ERR_LOG("Cannot create frame command buffer");
            return nullptr;
        }
        unique_ptr<render::QueryPool> timestamps;
        RenderBuffer timing;
        if (d.GetCapabilities().Features.TimestampQueries) {
            timestamps = d.CreateQueryPool({render::QueryType::Timestamp, 2, "Runtime graph"}).Release();
            timing = CreateBuffer(d, 16, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead).Release();
            if (!timestamps || !timing) {
                RADRAY_ERR_LOG("Cannot create frame timestamps");
                return nullptr;
            }
        }
        frames[i] = unique_ptr<FrameContext>(new FrameContext(d, i, std::move(allocator), std::move(command), std::move(timestamps), std::move(timing)));
    }
    return unique_ptr<Renderer>(new Renderer(device, std::move(pipeline), std::move(completion), std::move(frames)));
}
Renderer::~Renderer() {
    if (_submitted && _completion->GetCompletedValue() < _submitted) _device.GetQueue().Wait();
    for (auto& frame : _frames) frame->_allocator->Reset();
}
bool Renderer::Finish(uint32_t timeout) {
    if (_submitted && !_completion->WaitFor(_submitted, timeout)) {
        RADRAY_ERR_LOG("Renderer completion timed out; resources remain live");
        return false;
    }
    for (auto& f : _frames) {
        f->_allocator->Reset();
        f->_execution.reset();
        f->_keepAlive.clear();
    }
    return true;
}
std::span<const ExtractedResource> Renderer::Extractions() const {
    if (!_submitted) return {};
    const auto& e = _frames[(_submitted - 1) % 2]->_execution;
    return e ? std::span<const ExtractedResource>(e->Extractions()) : std::span<const ExtractedResource>{};
}
bool Renderer::Render(Scene& scene, const SceneUpdate& changes, const ViewSubmission& view, uint64_t bindingVersion, Nullable<const void*> data, const Submission& submission) {
    if (_submitted == std::numeric_limits<uint64_t>::max()) {
        RADRAY_ERR_LOG("Renderer completion exhausted");
        return false;
    }
    for (const auto& point : submission.waits)
        if (!point.fence) {
            RADRAY_ERR_LOG("Null wait fence");
            return false;
        }
    for (const auto& point : submission.signals)
        if (!point.fence || !point.value) {
            RADRAY_ERR_LOG("Invalid signal fence");
            return false;
        }
    Scene::Update update;
    if (!scene.Prepare(changes, view, _pipeline->Processor(), bindingVersion, update)) return false;
    auto& frame = *_frames[_submitted % 2];
    if (frame._completion && !_completion->WaitFor(frame._completion, submission.timeoutMs)) {
        RADRAY_ERR_LOG("Frame slot completion timed out; resources remain live");
        return false;
    }
    if (frame._measured) {
        auto* ticks = static_cast<uint64_t*>(frame._timing->Map(0, 16));
        if (!ticks) {
            RADRAY_ERR_LOG("Cannot read GPU timing");
            return false;
        }
        frame._timing->InvalidateMappedRange({0, 16});
        const auto elapsed = double(ticks[1] - ticks[0]) * frame._timestamps->GetTimestampCalibration(&_device.GetQueue()).TickPeriodNs / 1e6;
        frame._timing->Unmap();
        frame._measured = false;
        _gpuSamples.push_back(elapsed);
        if (_gpuSamples.size() == 120) {
            std::sort(_gpuSamples.begin(), _gpuSamples.end());
            RADRAY_INFO_LOG("Runtime graph GPU samples=120 ms_p50={} p95={} p99={} page_bytes={} command_builds={}",
                            _gpuSamples[60], _gpuSamples[114], _gpuSamples[118], _geometry.ResidentBytes(), scene.CommandBuilds());
            _gpuSamples.clear();
        }
    }
    frame._allocator->Reset();
    frame._execution.reset();
    frame._keepAlive.clear();
    GeometryStore::Update geometry;
    if (!_geometry.Prepare(changes, geometry, frame._staging, frame._stagingSize)) return false;
    PreparedMeshPass prepared;
    prepared.visible = update.visible;
    prepared.direct = submission.forceDirect || !_device.SupportsIndirect();
    vector<ResolvedDraw> resolved;
    for (const auto& visible : update.visible) {
        const auto& command = visible.Command();
        const auto& mesh = scene.Mesh(update, command.mesh);
        uint64_t page = 0;
        int32_t base = 0;
        if (mesh.stride) {
            auto allocation = _geometry.Mesh(geometry, command.mesh);
            page = allocation->page->id;
            base = static_cast<int32_t>(allocation->first);
            prepared.pages.emplace(page, BufferBinding(allocation->page->buffer.get(), Access::Vertex, render::BufferState::Vertex, allocation));
            geometry.keepAlive.push_back(allocation);
        }
        if (mesh.indexBuffer) {
            auto allocation = _geometry.Index(geometry, mesh.indexBuffer);
            prepared.indices.emplace(mesh.indexBuffer, BufferBinding(allocation->buffer.get(), Access::Index, render::BufferState::Index, allocation));
            geometry.keepAlive.push_back(allocation);
        }
        resolved.push_back({&command, page, mesh.indexBuffer, base, visible.item.depth_key, visible.item.first_instance, visible.item.instance_count});
    }
    if (!BuildDrawList(std::move(resolved), view.instances, view.instanceStride, prepared.draws)) return false;
    if (!prepared.draws.instances.empty()) {
        auto b = frame.Upload("Instances", prepared.draws.instances, render::BufferUse::Vertex);
        if (!b) return false;
        prepared.instances = BufferBinding(b.Get(), Access::Vertex, render::BufferState::Vertex);
    }
    if (!prepared.direct && !prepared.draws.indirect.empty()) {
        auto b = frame.Upload("Indirect", prepared.draws.indirect, render::BufferUse::Indirect);
        if (!b) return false;
        prepared.indirect = BufferBinding(b.Get(), Access::Indirect, render::BufferState::Indirect);
    }
    GraphBuilder graph;
    _geometry.AddUploadPass(graph, geometry, frame._staging.get());
    if (!_pipeline->BuildGraph(graph, frame, prepared, data)) return false;
    const bool measure = submission.profile && frame._timestamps;
    if (measure) {
        const auto readback = graph.ImportBuffer("GPU timing", frame._timing.get(), {ResourceState::Buffer(frame._timingInitialized ? render::BufferState::HostRead : render::BufferState::Undefined), ResourceState::Buffer(render::BufferState::HostRead), frame._timingInitialized});
        graph.AddCopyPass("TimestampResolve", {{readback, Access::CopyDestination}}, [readback, query = frame._timestamps.get()](PassContext& c) {
            c.Copy()->WriteTimestamp({query, render::QueryPipelineStage::Bottom, 1});
            c.Copy()->ResolveQueryData({query, 0, 2, c.Buffer(readback).Get(), 0});
            return true;
        });
    }
    CompiledGraph compiled;
    if (!graph.Compile(compiled)) return false;
    auto dump = compiled.Dump();
    if (!GetEnv("RADRAY_DUMP_GRAPH").empty()) RADRAY_INFO_LOG("{}", dump);
    unique_ptr<GraphExecution> execution;
    execution = GraphExecution::Create(_device.GetDevice(), _pool, std::move(compiled)).Release();
    if (!execution) return false;
    // Allocate all submission bookkeeping before recording or publishing any state.
    vector<render::Fence*> waits, signals{_completion.get()};
    vector<uint64_t> waitValues, signalValues{_submitted + 1};
    for (auto p : submission.waits) {
        waits.push_back(p.fence.Get());
        waitValues.push_back(p.value);
    }
    for (auto p : submission.signals) {
        signals.push_back(p.fence.Get());
        signalValues.push_back(p.value);
    }
    frame._keepAlive.insert(frame._keepAlive.end(), geometry.keepAlive.begin(), geometry.keepAlive.end());
    if (!execution->Record(*frame._command, measure ? frame._timestamps.get() : nullptr)) {
        frame._allocator->Reset();
        return false;
    }
    render::CommandBuffer* commands[]{frame._command.get()};
    _device.GetQueue().Submit({.CmdBuffers = commands, .SignalFences = signals, .SignalValues = signalValues, .WaitFences = waits, .WaitValues = waitValues});
    frame._completion = ++_submitted;
    execution->Submitted(_completion, _submitted);
    frame._measured = measure;
    frame._timingInitialized |= measure;
    frame._execution = std::move(execution);
    _geometry.Commit(geometry, update);
    scene.Commit(std::move(update));
    _lastGraph = std::move(dump);
    return true;
}
}  // namespace radray
