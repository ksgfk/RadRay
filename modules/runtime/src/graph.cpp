#include <radray/runtime/graph.h>

#include <algorithm>

#include <fmt/format.h>

#include <radray/logger.h>
#include <radray/runtime/renderer.h>

namespace radray {
namespace {
bool Reads(Access a) { return a != Access::CopyDestination && a != Access::StorageWrite && a != Access::Color && a != Access::DepthWrite; }
bool Writes(Access a) { return a == Access::CopyDestination || a == Access::StorageWrite || a == Access::StorageReadWrite || a == Access::Color || a == Access::DepthWrite; }
bool Uav(Access a) { return a == Access::StorageRead || a == Access::StorageWrite || a == Access::StorageReadWrite; }
bool Undefined(const GraphResource& r, const ResourceState& s) { return r.kind == ResourceKind::Buffer ? s.buffer == render::BufferState::Undefined : s.texture == render::TextureState::Undefined; }
ResourceState State(Access a, render::ShaderStages stages) {
    switch (a) {
        case Access::CopySource: return {render::BufferState::CopySource, render::TextureState::CopySource, {}};
        case Access::CopyDestination: return {render::BufferState::CopyDestination, render::TextureState::CopyDestination, {}};
        case Access::Vertex: return ResourceState::Buffer(render::BufferState::Vertex);
        case Access::Index: return ResourceState::Buffer(render::BufferState::Index);
        case Access::Constant: return ResourceState::Buffer(render::BufferState::CBuffer, stages);
        case Access::Indirect: return ResourceState::Buffer(render::BufferState::Indirect);
        case Access::Sample: return {render::BufferState::ShaderRead, render::TextureState::ShaderRead, stages};
        case Access::StorageRead:
        case Access::StorageWrite:
        case Access::StorageReadWrite:
            return {render::BufferState::UnorderedAccess, render::TextureState::UnorderedAccess, stages};
        case Access::Color: return ResourceState::Texture(render::TextureState::RenderTarget);
        case Access::DepthRead: return ResourceState::Texture(render::TextureState::DepthRead);
        case Access::DepthWrite: return ResourceState::Texture(render::TextureState::DepthWrite);
    }
    return {};
}
bool Compatible(const GraphResource& r, Access a) {
    if (r.kind == ResourceKind::Buffer) {
        render::BufferUse use = render::BufferUse::UNKNOWN;
        switch (a) {
            case Access::CopySource: use = render::BufferUse::CopySource; break;
            case Access::CopyDestination: use = render::BufferUse::CopyDestination; break;
            case Access::Vertex: use = render::BufferUse::Vertex; break;
            case Access::Index: use = render::BufferUse::Index; break;
            case Access::Constant: use = render::BufferUse::CBuffer; break;
            case Access::Indirect: use = render::BufferUse::Indirect; break;
            case Access::Sample: use = render::BufferUse::Resource; break;
            case Access::StorageRead:
            case Access::StorageWrite:
            case Access::StorageReadWrite: use = render::BufferUse::UnorderedAccess; break;
            default: return false;
        }
        return r.buffer.Usage.HasFlag(use);
    }
    render::TextureUse use = render::TextureUse::UNKNOWN;
    switch (a) {
        case Access::CopySource: use = render::TextureUse::CopySource; break;
        case Access::CopyDestination: use = render::TextureUse::CopyDestination; break;
        case Access::Sample: use = render::TextureUse::Resource; break;
        case Access::StorageRead:
        case Access::StorageWrite:
        case Access::StorageReadWrite: use = render::TextureUse::UnorderedAccess; break;
        case Access::Color: use = render::TextureUse::RenderTarget; break;
        case Access::DepthRead: use = render::TextureUse::DepthStencilRead; break;
        case Access::DepthWrite: use = render::TextureUse::DepthStencilWrite; break;
        default: return false;
    }
    return r.texture.Usage.HasFlag(use);
}
bool SameState(const GraphResource& r, const ResourceState& a, const ResourceState& b) {
    return (r.kind == ResourceKind::Buffer ? a.buffer == b.buffer : a.texture == b.texture) && a.stages == b.stages;
}
void RetainOwner(ImportOptions& target, shared_ptr<void> owner) {
    if (!owner) return;
    if (!target.owner) {
        target.owner = std::move(owner);
        return;
    }
    if (!target.owner.owner_before(owner) && !owner.owner_before(target.owner)) return;
    struct Owners {
        shared_ptr<void> first, second;
    };
    target.owner = make_shared<Owners>(Owners{target.owner, std::move(owner)});
}
}  // namespace
ResourceState ResourceState::Buffer(render::BufferStates s, render::ShaderStages stages) {
    return {s, render::TextureState::Undefined, stages};
}

ResourceState ResourceState::Texture(render::TextureStates s, render::ShaderStages stages) {
    return {render::BufferState::Undefined, s, stages};
}

GraphBuilder::GraphBuilder() : _id(make_shared<GraphIdentity>()) {}
std::optional<ResourceHandle> GraphBuilder::Find(Nullable<const render::Resource*> resource) const {
    if (!resource) return {};
    for (uint32_t i = 0; i < _resources.size(); ++i)
        if (_resources[i].importedBuffer.Get() == resource.Get() || _resources[i].importedTexture.Get() == resource.Get()) return ResourceHandle{_id, i};
    return {};
}
ResourceHandle GraphBuilder::AddResource(GraphResource r) {
    const ResourceHandle h{_id, static_cast<uint32_t>(_resources.size())};
    _resources.push_back(std::move(r));
    return h;
}
TextureHandle GraphBuilder::CreateTexture(string name, const render::TextureDescriptor& d) {
    GraphResource r{std::move(name), ResourceKind::Texture};
    r.texture = d;
    return AddResource(std::move(r));
}
BufferHandle GraphBuilder::CreateBuffer(string name, const render::BufferDescriptor& d) {
    GraphResource r{std::move(name), ResourceKind::Buffer};
    r.buffer = d;
    return AddResource(std::move(r));
}
TextureHandle GraphBuilder::ImportTexture(string name, Nullable<render::Texture*> t, ImportOptions options) {
    if (!t) {
        RADRAY_ERR_LOG("Cannot import a null texture");
        _failed = true;
        return {};
    }
    for (uint32_t i = 0; i < _resources.size(); ++i)
        if (_resources[i].importedTexture == t) {
            auto& old = _resources[i].import;
            if (old.initial != options.initial || old.final != options.final || old.initialized != options.initialized || old.acquire != options.acquire || old.release != options.release) {
                RADRAY_ERR_LOG("Conflicting duplicate texture import");
                _failed = true;
            }
            RetainOwner(old, std::move(options.owner));
            return {_id, i};
        }
    GraphResource r{std::move(name), ResourceKind::Texture};
    r.texture = t->GetDesc();
    r.importedTexture = t;
    r.imported = true;
    r.import = std::move(options);
    return AddResource(std::move(r));
}
BufferHandle GraphBuilder::ImportBuffer(string name, Nullable<render::Buffer*> b, ImportOptions options) {
    if (!b) {
        RADRAY_ERR_LOG("Cannot import a null buffer");
        _failed = true;
        return {};
    }
    for (uint32_t i = 0; i < _resources.size(); ++i)
        if (_resources[i].importedBuffer == b) {
            auto& old = _resources[i].import;
            if (old.initial != options.initial || old.final != options.final || old.initialized != options.initialized || old.acquire != options.acquire || old.release != options.release) {
                RADRAY_ERR_LOG("Conflicting duplicate buffer import");
                _failed = true;
            }
            RetainOwner(old, std::move(options.owner));
            return {_id, i};
        }
    GraphResource r{std::move(name), ResourceKind::Buffer};
    r.buffer = b->GetDesc();
    r.importedBuffer = b;
    r.imported = true;
    r.import = std::move(options);
    return AddResource(std::move(r));
}
void GraphBuilder::MarkOutput(ResourceHandle h) {
    if (h.graph != _id || h.index >= _resources.size()) {
        RADRAY_ERR_LOG("Output has a foreign or invalid graph handle");
        _failed = true;
    } else
        _resources[h.index].output = true;
}
void GraphBuilder::Extract(ResourceHandle h) {
    MarkOutput(h);
    if (h.graph == _id && h.index < _resources.size()) _resources[h.index].extracted = true;
}
uint32_t GraphBuilder::AddPass(GraphPass p) {
    const auto n = static_cast<uint32_t>(_passes.size());
    _passes.push_back(std::move(p));
    return n;
}
uint32_t GraphBuilder::AddRasterPass(string n, vector<ResourceAccess> a, RasterAttachments attachments, PassCallback f, bool side) {
    for (const auto& c : attachments.colors) a.push_back({c.texture, Access::Color, {}});
    if (attachments.depth) a.push_back({attachments.depth->texture, attachments.depth->readOnly ? Access::DepthRead : Access::DepthWrite, {}});
    return AddPass({std::move(n), PassKind::Raster, std::move(a), std::move(attachments), std::move(f), side});
}
uint32_t GraphBuilder::AddComputePass(string n, vector<ResourceAccess> a, PassCallback f, bool side) {
    return AddPass({std::move(n), PassKind::Compute, std::move(a), {}, std::move(f), side});
}
uint32_t GraphBuilder::AddCopyPass(string n, vector<ResourceAccess> a, PassCallback f, bool side) {
    return AddPass({std::move(n), PassKind::Copy, std::move(a), {}, std::move(f), side});
}
bool GraphBuilder::Compile(CompiledGraph& output) const {
    if (_failed) return false;
    CompiledGraph g;
    g.id = _id;
    g.resources = _resources;
    g.passes = _passes;
    const auto count = _resources.size();
    vector<int32_t> writer(count, -1);
    vector<vector<uint32_t>> readers(count), dependencies(_passes.size());
    vector<bool> initialized(count), roots(_passes.size()), live(_passes.size());
    for (size_t i = 0; i < count; ++i) {
        const auto& r = _resources[i];
        if (r.kind == ResourceKind::Buffer ? !r.buffer.Size : (!r.texture.Width || !r.texture.Height || !r.texture.DepthOrArraySize || !r.texture.MipLevels || !r.texture.SampleCount || r.texture.Format == render::TextureFormat::UNKNOWN)) {
            RADRAY_ERR_LOG("Invalid resource descriptor: {}", r.name);
            return false;
        }
        initialized[i] = r.imported && r.import.initialized && !Undefined(r, r.import.initial);
    }
    for (uint32_t pi = 0; pi < g.passes.size(); ++pi) {
        auto& p = g.passes[pi];
        roots[pi] = p.sideEffect;
        map<uint32_t, ResourceAccess> unique;
        for (auto a : p.accesses) {
            if (a.resource.graph != _id || a.resource.index >= count) {
                RADRAY_ERR_LOG("Foreign or invalid graph handle in {}", p.name);
                return false;
            }
            const auto ri = a.resource.index;
            const auto& r = _resources[ri];
            if (!Compatible(r, a.access)) {
                RADRAY_ERR_LOG("Resource usage incompatible with access: {} / {}", p.name, r.name);
                return false;
            }
            if (p.kind == PassKind::Copy && a.access != Access::CopySource && a.access != Access::CopyDestination) {
                RADRAY_ERR_LOG("Non-copy access in copy pass: {}", p.name);
                return false;
            }
            if (p.kind == PassKind::Compute && (a.access == Access::Vertex || a.access == Access::Index || a.access == Access::Color || a.access == Access::DepthRead || a.access == Access::DepthWrite)) {
                RADRAY_ERR_LOG("Raster access in compute pass: {}", p.name);
                return false;
            }
            if (p.kind != PassKind::Copy && (a.access == Access::CopySource || a.access == Access::CopyDestination)) {
                RADRAY_ERR_LOG("Copy access outside copy pass: {}", p.name);
                return false;
            }
            const bool shader = a.access == Access::Sample || a.access == Access::Constant || Uav(a.access);
            if (shader && !a.stages) a.stages = p.kind == PassKind::Compute ? render::ShaderStages(render::ShaderStage::Compute) : render::ShaderStage::Vertex | render::ShaderStage::Pixel;
            if (shader && (p.kind == PassKind::Compute ? a.stages != render::ShaderStage::Compute : a.stages.HasFlag(render::ShaderStage::Compute))) {
                RADRAY_ERR_LOG("Shader stage incompatible with pass: {}", p.name);
                return false;
            }
            auto [it, inserted] = unique.emplace(ri, a);
            if (!inserted) {
                if (it->second.access != a.access || a.access == Access::Color || a.access == Access::DepthWrite) {
                    RADRAY_ERR_LOG("Conflicting accesses in pass: {}", p.name);
                    return false;
                }
                it->second.stages |= a.stages;
            }
        }
        if (p.kind == PassKind::Raster) {
            if (!p.attachments.width || !p.attachments.height || (p.attachments.colors.empty() && !p.attachments.depth)) {
                RADRAY_ERR_LOG("Raster pass has no valid attachments: {}", p.name);
                return false;
            }
            for (const auto& [ri, a] : unique) {
                if (a.access == Access::Color && std::none_of(p.attachments.colors.begin(), p.attachments.colors.end(), [&](const auto& c) { return c.texture.index == ri; })) {
                    RADRAY_ERR_LOG("Color access has no attachment: {}", p.name);
                    return false;
                }
                if ((a.access == Access::DepthRead || a.access == Access::DepthWrite) && (!p.attachments.depth || p.attachments.depth->texture.index != ri)) {
                    RADRAY_ERR_LOG("Depth access has no attachment: {}", p.name);
                    return false;
                }
            }
            uint32_t samples = 0;
            const auto attachment = [&](ResourceHandle h, bool depth, render::LoadAction load, render::StoreAction store) {
                const auto& r = _resources[h.index];
                if (r.kind != ResourceKind::Texture || r.texture.Dim != render::TextureDimension::Dim2D || r.texture.Width != p.attachments.width || r.texture.Height != p.attachments.height || r.texture.DepthOrArraySize != 1 ||
                    (samples && samples != r.texture.SampleCount) || (depth != render::IsDepthStencilFormat(r.texture.Format))) return false;
                samples = r.texture.SampleCount;
                if (load != render::LoadAction::Load && load != render::LoadAction::Clear && load != render::LoadAction::DontCare) return false;
                if (store != render::StoreAction::Store && store != render::StoreAction::Discard) return false;
                return load != render::LoadAction::Load || initialized[h.index];
            };
            for (const auto& a : p.attachments.colors)
                if (!attachment(a.texture, false, a.load, a.store)) {
                    RADRAY_ERR_LOG("Invalid color attachment or uninitialized Load: {}", p.name);
                    return false;
                }
            if (p.attachments.depth) {
                const auto& a = *p.attachments.depth;
                if (!attachment(a.texture, true, a.load, a.store) || (a.readOnly && a.load != render::LoadAction::Load)) {
                    RADRAY_ERR_LOG("Invalid depth attachment: {}", p.name);
                    return false;
                }
            }
        }
        p.accesses.clear();
        set<uint32_t> deps;
        for (const auto& [ri, a] : unique) {
            if (Reads(a.access) && !initialized[ri]) {
                RADRAY_ERR_LOG("Uninitialized read: {} / {}", p.name, _resources[ri].name);
                return false;
            }
            if (writer[ri] >= 0) deps.insert(static_cast<uint32_t>(writer[ri]));
            if (Writes(a.access)) {
                deps.insert(readers[ri].begin(), readers[ri].end());
                readers[ri].clear();
                writer[ri] = static_cast<int32_t>(pi);
                initialized[ri] = true;
                roots[pi] = roots[pi] || _resources[ri].imported;
            } else
                readers[ri].push_back(pi);
            p.accesses.push_back(a);
        }
        for (const auto& a : p.attachments.colors)
            if (a.store == render::StoreAction::Discard) initialized[a.texture.index] = false;
        if (p.attachments.depth && p.attachments.depth->store == render::StoreAction::Discard) initialized[p.attachments.depth->texture.index] = false;
        dependencies[pi] = {deps.begin(), deps.end()};
    }
    for (uint32_t i = 0; i < count; ++i)
        if (_resources[i].output) {
            if (!initialized[i]) {
                RADRAY_ERR_LOG("Uninitialized output: {}", _resources[i].name);
                return false;
            }
            if (writer[i] >= 0) roots[writer[i]] = true;
        }
    vector<uint32_t> stack;
    for (uint32_t i = 0; i < roots.size(); ++i)
        if (roots[i]) stack.push_back(i);
    while (!stack.empty()) {
        auto i = stack.back();
        stack.pop_back();
        if (live[i]) continue;
        live[i] = true;
        for (auto d : dependencies[i]) stack.push_back(d);
    }
    vector<ResourceState> states(count);
    vector<bool> used(count), lastUav(count), lastWrite(count);
    g.lifetimes.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        if (_resources[i].imported) states[i] = _resources[i].import.initial;
    for (uint32_t pi = 0; pi < g.passes.size(); ++pi)
        if (live[pi]) {
            PlannedPass plan{pi, dependencies[pi], {}};
            const auto pos = static_cast<int32_t>(g.plan.size());
            for (auto a : g.passes[pi].accesses) {
                const auto ri = a.resource.index;
                const auto& r = _resources[ri];
                auto after = State(a.access, a.stages);
                // Normalize the irrelevant state member so imported-state equality remains unambiguous.
                if (r.kind == ResourceKind::Buffer)
                    after.texture = render::TextureState::Undefined;
                else
                    after.buffer = render::BufferState::Undefined;
                const bool acquire = !used[ri] && r.imported && r.import.acquire;
                if (!SameState(r, states[ri], after) || acquire) plan.barriers.push_back({ri, states[ri], after, false, acquire ? render::ExternalOwnershipTransfer::Acquire : render::ExternalOwnershipTransfer::None});
                if (lastUav[ri] && Uav(a.access) && (lastWrite[ri] || Writes(a.access))) plan.barriers.push_back({ri, after, after, true});
                states[ri] = after;
                lastUav[ri] = Uav(a.access);
                lastWrite[ri] = Writes(a.access);
                if (!used[ri]) g.lifetimes[ri].first = pos;
                g.lifetimes[ri].last = pos;
                used[ri] = true;
            }
            g.plan.push_back(std::move(plan));
        }
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = _resources[i];
        if (!r.imported) continue;
        if (!used[i] && (r.import.acquire || r.import.release || r.output)) {
            g.lifetimes[i] = {static_cast<int32_t>(g.plan.size()), static_cast<int32_t>(g.plan.size())};
            if (r.import.acquire) g.epilogue.push_back({i, states[i], states[i], false, render::ExternalOwnershipTransfer::Acquire});
            used[i] = true;
        }
        if (used[i] && (!SameState(r, states[i], r.import.final) || r.import.release)) {
            if (Undefined(r, r.import.final)) {
                RADRAY_ERR_LOG("Imported final state cannot be undefined: {}", r.name);
                return false;
            }
            g.epilogue.push_back({i, states[i], r.import.final, false, r.import.release ? render::ExternalOwnershipTransfer::Release : render::ExternalOwnershipTransfer::None});
            states[i] = r.import.final;
        }
    }
    g.finalStates = std::move(states);
    output = std::move(g);
    return true;
}
string CompiledGraph::Dump() const {
    string output = fmt::format("graph {} passes={}/{}\n", fmt::ptr(id.get()), plan.size(), passes.size());
    for (const auto& p : plan) {
        output += fmt::format("pass {} {} deps=", p.index, passes[p.index].name);
        for (auto dependency : p.dependencies) output += fmt::format("{},", dependency);
        output += fmt::format(" barriers={}\n", p.barriers.size());
        for (const auto& b : p.barriers) output += fmt::format("  {} {} buffer={}->{} texture={}->{} stages={}->{} transfer={}\n",
                                                               resources[b.resource].name, b.uav ? "UAV" : "transition", b.before.buffer.value(), b.after.buffer.value(), b.before.texture.value(),
                                                               b.after.texture.value(), b.before.stages.value(), b.after.stages.value(), static_cast<int>(b.transfer));
    }
    for (size_t i = 0; i < resources.size(); ++i) output += fmt::format("resource {} {} lifetime={}:{}\n", i, resources[i].name, lifetimes[i].first, lifetimes[i].last);
    output += fmt::format("epilogue={}\n", epilogue.size());
    return output;
}
uint32_t GraphBuilder::AddMeshPass(string name, PreparedMeshPass& pass, RasterAttachments attachments) {
    auto accesses = pass.Declare(*this);
    return AddRasterPass(std::move(name), std::move(accesses), std::move(attachments), [prepared = pass](PassContext& c) { return prepared.Draw(c); });
}
}  // namespace radray
