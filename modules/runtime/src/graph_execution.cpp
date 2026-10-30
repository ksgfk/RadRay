#include <radray/runtime/graph_execution.h>

#include <algorithm>
#include <tuple>

#include <radray/logger.h>
#include <radray/scope_guard.h>

namespace radray {
namespace {
bool Equal(const render::BufferDescriptor& a, const render::BufferDescriptor& b) { return std::tie(a.Size, a.Memory, a.Usage, a.Hints) == std::tie(b.Size, b.Memory, b.Usage, b.Hints); }
bool Equal(const render::TextureDescriptor& a, const render::TextureDescriptor& b) {
    return std::tie(a.Dim, a.Width, a.Height, a.DepthOrArraySize, a.MipLevels, a.SampleCount, a.Format, a.Memory, a.Usage, a.Hints) ==
           std::tie(b.Dim, b.Width, b.Height, b.DepthOrArraySize, b.MipLevels, b.SampleCount, b.Format, b.Memory, b.Usage, b.Hints);
}
}  // namespace
ResourceStore::ResourceStore(render::Device& device) : _device(device) {}

Nullable<shared_ptr<ResourceAllocation>> ResourceStore::Acquire(const GraphResource& r) {
    for (auto& allocation : _pool)
        if (allocation.use_count() == 1 && (!allocation->completion || allocation->completion->GetCompletedValue() >= allocation->value) &&
            (r.kind == ResourceKind::Buffer ? allocation->buffer && Equal(allocation->buffer->GetDesc(), r.buffer) : allocation->texture && Equal(allocation->texture->GetDesc(), r.texture))) return allocation;
    auto allocation = make_shared<ResourceAllocation>();
    if (r.kind == ResourceKind::Buffer)
        allocation->buffer = _device.CreateBuffer(r.buffer).Release();
    else
        allocation->texture = _device.CreateTexture(r.texture).Release();
    if (!allocation->buffer && !allocation->texture) return {};
    _pool.push_back(allocation);
    return allocation;
}
GraphExecution::GraphExecution(render::Device& d, CompiledGraph g) : _device(d), _graph(std::move(g)) {}

Nullable<unique_ptr<GraphExecution>> GraphExecution::Create(render::Device& d, ResourceStore& pool, CompiledGraph graph) {
    auto e = unique_ptr<GraphExecution>(new GraphExecution(d, std::move(graph)));
    const auto size = e->_graph.resources.size();
    e->_allocations.resize(size);
    e->_buffers.resize(size);
    e->_textures.resize(size);
    for (uint32_t i = 0; i < size; ++i) {
        const auto& r = e->_graph.resources[i];
        if (e->_graph.lifetimes[i].first < 0) continue;
        if (r.imported) {
            e->_buffers[i] = r.importedBuffer;
            e->_textures[i] = r.importedTexture;
            if (r.import.owner) e->_owners.push_back(r.import.owner);
        } else {
            auto allocation = pool.Acquire(r);
            if (!allocation) {
                RADRAY_ERR_LOG("Cannot allocate graph resource: {}", r.name);
                return nullptr;
            }
            e->_buffers[i] = allocation->buffer.get();
            e->_textures[i] = allocation->texture.get();
            auto& first = e->_graph.plan.at(e->_graph.lifetimes[i].first);
            for (auto& barrier : first.barriers)
                if (barrier.resource == i) {
                    barrier.before = allocation->state;
                    break;
                }
            e->_allocations[i] = allocation.Release();
        }
        if (r.extracted) e->_extractions.push_back({{e->_graph.id, i}, e->_allocations[i], r.import.owner, e->_buffers[i], e->_textures[i], e->_graph.finalStates[i]});
    }
    return e;
}
void GraphExecution::Barriers(render::CommandBuffer& command, std::span<const PlannedBarrier> barriers) {
    for (const auto& b : barriers) {
        render::ResourceBarrierDescriptor descriptor;
        if (b.uav)
            descriptor = render::BarrierUavDescriptor{_buffers[b.resource] ? static_cast<render::Resource*>(_buffers[b.resource].Get()) : static_cast<render::Resource*>(_textures[b.resource].Get())};
        else if (_buffers[b.resource])
            descriptor = render::BarrierBufferDescriptor{.Target = _buffers[b.resource].Get(), .Before = b.before.buffer, .After = b.after.buffer, .BeforeStages = b.before.stages, .AfterStages = b.after.stages, .ExternalTransfer = b.transfer};
        else
            descriptor = render::BarrierTextureDescriptor{.Target = _textures[b.resource].Get(), .Before = b.before.texture, .After = b.after.texture, .BeforeStages = b.before.stages, .AfterStages = b.after.stages, .ExternalTransfer = b.transfer};
        command.ResourceBarrier({&descriptor, 1});
    }
}
Nullable<render::TextureView*> GraphExecution::View(uint32_t i, render::TextureViewUsage usage) {
    auto* t = _textures.at(i).Get();
    const auto& d = t->GetDesc();
    const bool attachment = usage == render::TextureViewUsage::RenderTarget || usage == render::TextureViewUsage::DepthWrite || usage == render::TextureViewUsage::DepthRead;
    auto view = _device.CreateTextureView({t, d.Dim, d.Format, {0, d.DepthOrArraySize, 0, attachment ? 1u : d.MipLevels}, usage}).Release();
    if (!view) return nullptr;
    auto* result = view.get();
    _views.push_back(std::move(view));
    return result;
}
bool GraphExecution::Record(render::CommandBuffer& command, Nullable<render::QueryPool*> timestamps) {
    command.Begin();
    auto end = MakeScopeGuard([&]() noexcept { command.End(); });
    if (timestamps) {
        command.ResetQueryPool(timestamps.Get(), 0, 2);
        command.WriteTimestamp({timestamps.Get(), render::QueryPipelineStage::Top, 0});
    }
    for (const auto& planned : _graph.plan) {
        const auto& p = _graph.passes[planned.index];
        Barriers(command, planned.barriers);
        unique_ptr<render::GraphicsCommandEncoder> raster;
        unique_ptr<render::ComputeCommandEncoder> compute;
        auto close = MakeScopeGuard([&]() noexcept {
            if (raster) command.EndRenderPass(std::move(raster));
            if (compute) command.EndComputePass(std::move(compute));
        });
        if (p.kind == PassKind::Raster) {
            vector<render::RenderPassColorAttachment> colors;
            for (const auto& a : p.attachments.colors) {
                auto view = View(a.texture.index, render::TextureViewUsage::RenderTarget);
                if (!view) {
                    RADRAY_ERR_LOG("Cannot create color attachment view");
                    return false;
                }
                colors.push_back({view.Get(), a.load, a.store, a.clear});
            }
            std::optional<render::RenderPassDepthStencilAttachment> depth;
            if (p.attachments.depth) {
                const auto& a = *p.attachments.depth;
                auto view = View(a.texture.index, a.readOnly ? render::TextureViewUsage::DepthRead : render::TextureViewUsage::DepthWrite);
                if (!view) {
                    RADRAY_ERR_LOG("Cannot create depth attachment view");
                    return false;
                }
                depth = render::RenderPassDepthStencilAttachment{.View = view.Get(), .DepthLoad = a.load, .DepthStore = a.store, .StencilLoad = render::LoadAction::DontCare, .StencilStore = render::StoreAction::Discard, .Clear = {a.clear, 0}};
            }
            const bool uav = std::any_of(p.accesses.begin(), p.accesses.end(), [](const auto& a) { return a.access == Access::StorageWrite || a.access == Access::StorageReadWrite; });
            raster = command.BeginRenderPass({.Colors = colors, .DepthStencil = depth, .Width = p.attachments.width, .Height = p.attachments.height, .Name = p.name, .AllowUavWrites = uav}).Release();
            if (!raster) {
                RADRAY_ERR_LOG("Cannot begin raster pass: {}", p.name);
                return false;
            }
            raster->SetViewport({0, 0, float(p.attachments.width), float(p.attachments.height), 0, 1});
            raster->SetScissor({0, 0, p.attachments.width, p.attachments.height});
        } else if (p.kind == PassKind::Compute) {
            compute = command.BeginComputePass().Release();
            if (!compute) {
                RADRAY_ERR_LOG("Cannot begin compute pass: {}", p.name);
                return false;
            }
        }
        PassContext context(*this, p, command, raster.get(), compute.get());
        if (p.execute && !p.execute(context)) return false;
        if (context._failed) return false;
    }
    Barriers(command, _graph.epilogue);
    return true;
}
void GraphExecution::Submitted(shared_ptr<render::Fence> fence, uint64_t value) {
    for (uint32_t i = 0; i < _allocations.size(); ++i)
        if (_allocations[i]) {
            _allocations[i]->state = _graph.finalStates[i];
            _allocations[i]->completion = fence;
            _allocations[i]->value = value;
        }
    for (auto& e : _extractions) {
        e.completion = fence;
        e.value = value;
    }
}
PassContext::PassContext(GraphExecution& e, const GraphPass& p, render::CommandBuffer& c, Nullable<render::GraphicsCommandEncoder*> r, Nullable<render::ComputeCommandEncoder*> compute)
    : _execution(e), _pass(p), _command(c), _raster(r), _compute(compute) {}
bool PassContext::Check(ResourceHandle h, ResourceKind kind) const {
    if (h.graph != _execution._graph.id || h.index >= _execution._graph.resources.size() || _execution._graph.resources[h.index].kind != kind ||
        std::none_of(_pass.accesses.begin(), _pass.accesses.end(), [&](const auto& a) { return a.resource == h; })) {
        RADRAY_ERR_LOG("Pass attempted to resolve an undeclared resource: {}", _pass.name);
        _failed = true;
        return false;
    }
    return true;
}
Nullable<render::Buffer*> PassContext::Buffer(BufferHandle h) const { return Check(h, ResourceKind::Buffer) ? _execution._buffers[h.index] : nullptr; }
Nullable<render::Texture*> PassContext::Texture(TextureHandle h) const { return Check(h, ResourceKind::Texture) ? _execution._textures[h.index] : nullptr; }
Nullable<render::GraphicsCommandEncoder*> PassContext::Raster() const {
    if (!_raster) {
        RADRAY_ERR_LOG("Not a raster pass: {}", _pass.name);
        _failed = true;
    }
    return _raster;
}
Nullable<render::ComputeCommandEncoder*> PassContext::Compute() const {
    if (!_compute) {
        RADRAY_ERR_LOG("Not a compute pass: {}", _pass.name);
        _failed = true;
    }
    return _compute;
}
Nullable<render::CommandBuffer*> PassContext::Copy() const {
    if (_pass.kind != PassKind::Copy) {
        RADRAY_ERR_LOG("Not a copy pass: {}", _pass.name);
        _failed = true;
        return nullptr;
    }
    return &_command;
}
render::Device& PassContext::Device() const { return _execution._device; }
Nullable<render::TextureView*> PassContext::CreateView(TextureHandle h, render::TextureViewUsage usage) {
    if (!Check(h, ResourceKind::Texture)) return nullptr;
    const auto access = std::find_if(_pass.accesses.begin(), _pass.accesses.end(), [&](const auto& a) { return a.resource == h; })->access;
    const bool compatible = (usage == render::TextureViewUsage::Resource && access == Access::Sample) ||
                            (usage == render::TextureViewUsage::UnorderedAccess && (access == Access::StorageRead || access == Access::StorageWrite || access == Access::StorageReadWrite)) ||
                            (usage == render::TextureViewUsage::RenderTarget && access == Access::Color) || (usage == render::TextureViewUsage::DepthRead && access == Access::DepthRead) ||
                            (usage == render::TextureViewUsage::DepthWrite && access == Access::DepthWrite);
    if (!compatible) {
        RADRAY_ERR_LOG("Texture view usage was not declared by this pass: {}", _pass.name);
        _failed = true;
        return nullptr;
    }
    auto view = _execution.View(h.index, usage);
    if (!view) {
        RADRAY_ERR_LOG("Cannot create graph texture view: {}", _pass.name);
        _failed = true;
    }
    return view;
}
void PassContext::KeepAlive(shared_ptr<void> owner) { _execution._owners.push_back(std::move(owner)); }
}  // namespace radray
