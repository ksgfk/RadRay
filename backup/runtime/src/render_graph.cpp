#include <queue>
#include <unordered_map>

#include <radray/runtime/render_graph.h>
#include <radray/runtime/renderer_runtime.h>

namespace radray::runtime {
namespace {

enum class ResourceAccess : uint32_t {
    ReadTexture = 0,
    WriteTexture,
    ReadBuffer,
    WriteBuffer,
    ColorAttachment,
    Present,
};

struct ResourceUsage {
    RgResourceHandle Resource{};
    ResourceAccess Access{ResourceAccess::ReadTexture};
};

struct ResourceRecord {
    string Name{};
    RgResourceLifetime Lifetime{RgResourceLifetime::External};
    RgResourceKind Kind{RgResourceKind::Texture};

    ImportedTextureDesc ImportedTexture{};
    ImportedBufferDesc ImportedBuffer{};
    render::TextureDescriptor TextureDesc{};
    render::BufferDescriptor BufferDesc{};
    PersistentTextureHandle PersistentTexture{};

    unique_ptr<render::Texture> TransientTexture{};
    unique_ptr<render::TextureView> TransientDefaultView{};
    unique_ptr<render::Buffer> TransientBuffer{};

    render::TextureState InitialTextureState{render::TextureState::UNKNOWN};
    render::TextureState FinalTextureState{render::TextureState::UNKNOWN};
    render::BufferState InitialBufferState{render::BufferState::UNKNOWN};
    render::BufferState FinalBufferState{render::BufferState::UNKNOWN};

    bool HasProducer{false};
    int32_t FirstUse{-1};
    int32_t LastUse{-1};
};

struct PassRecord {
    string Name{};
    RgPassType Type{RgPassType::Raster};
    vector<ResourceUsage> Usages{};
    RenderGraphExecuteFn Execute{};
};

struct ResolvedResource {
    render::Texture* Texture{nullptr};
    render::TextureView* TextureView{nullptr};
    render::Buffer* Buffer{nullptr};
};

bool IsWriteAccess(ResourceAccess access) noexcept {
    return access == ResourceAccess::WriteTexture ||
           access == ResourceAccess::WriteBuffer ||
           access == ResourceAccess::ColorAttachment ||
           access == ResourceAccess::Present;
}

bool IsTextureAccess(ResourceAccess access) noexcept {
    return access == ResourceAccess::ReadTexture ||
           access == ResourceAccess::WriteTexture ||
           access == ResourceAccess::ColorAttachment ||
           access == ResourceAccess::Present;
}

bool IsBufferAccess(ResourceAccess access) noexcept {
    return access == ResourceAccess::ReadBuffer || access == ResourceAccess::WriteBuffer;
}

render::TextureState GetRequiredTextureState(RgPassType passType, ResourceAccess access) noexcept {
    switch (access) {
        case ResourceAccess::ReadTexture:
            return passType == RgPassType::Copy ? render::TextureState::CopySource : render::TextureState::ShaderRead;
        case ResourceAccess::WriteTexture: return render::TextureState::CopyDestination;
        case ResourceAccess::ColorAttachment: return render::TextureState::RenderTarget;
        case ResourceAccess::Present: return render::TextureState::Present;
        default: return render::TextureState::UNKNOWN;
    }
}

render::BufferState GetRequiredBufferState(RgPassType passType, ResourceAccess access) noexcept {
    switch (access) {
        case ResourceAccess::ReadBuffer:
            return passType == RgPassType::Copy ? render::BufferState::CopySource : render::BufferState::ShaderRead;
        case ResourceAccess::WriteBuffer: return render::BufferState::CopyDestination;
        default: return render::BufferState::UNKNOWN;
    }
}

}  // namespace

class RenderGraphPassContext::Impl {
public:
    RenderFrameContext* FrameCtx{nullptr};
    render::CommandBuffer* CommandBuffer{nullptr};
    const std::unordered_map<uint32_t, ResolvedResource>* ResolvedResources{nullptr};
};

RenderFrameContext& RenderGraphPassContext::Frame() noexcept {
    return *_impl->FrameCtx;
}

const RenderFrameContext& RenderGraphPassContext::Frame() const noexcept {
    return *_impl->FrameCtx;
}

render::CommandBuffer* RenderGraphPassContext::Cmd() noexcept {
    return _impl->CommandBuffer;
}

render::Texture* RenderGraphPassContext::GetTexture(RgResourceHandle handle) noexcept {
    if (_impl->ResolvedResources == nullptr) {
        return nullptr;
    }
    const auto it = _impl->ResolvedResources->find(handle.Id);
    return it == _impl->ResolvedResources->end() ? nullptr : it->second.Texture;
}

render::TextureView* RenderGraphPassContext::GetTextureView(RgResourceHandle handle) noexcept {
    if (_impl->ResolvedResources == nullptr) {
        return nullptr;
    }
    const auto it = _impl->ResolvedResources->find(handle.Id);
    return it == _impl->ResolvedResources->end() ? nullptr : it->second.TextureView;
}

render::Buffer* RenderGraphPassContext::GetBuffer(RgResourceHandle handle) noexcept {
    if (_impl->ResolvedResources == nullptr) {
        return nullptr;
    }
    const auto it = _impl->ResolvedResources->find(handle.Id);
    return it == _impl->ResolvedResources->end() ? nullptr : it->second.Buffer;
}

RenderGraphPassContext::RenderGraphPassContext(Impl* impl) noexcept
    : _impl(impl) {}

class PassBuilder::Impl {
public:
    explicit Impl(PassRecord* pass) noexcept
        : Pass(pass) {}

    PassRecord* Pass{nullptr};
};

PassBuilder::PassBuilder(Impl* impl) noexcept
    : _impl(impl) {}

RgResourceHandle PassBuilder::ReadTexture(RgResourceHandle handle) {
    _impl->Pass->Usages.push_back(ResourceUsage{handle, ResourceAccess::ReadTexture});
    return handle;
}

RgResourceHandle PassBuilder::WriteTexture(RgResourceHandle handle) {
    _impl->Pass->Usages.push_back(ResourceUsage{handle, ResourceAccess::WriteTexture});
    return handle;
}

RgResourceHandle PassBuilder::ReadBuffer(RgResourceHandle handle) {
    _impl->Pass->Usages.push_back(ResourceUsage{handle, ResourceAccess::ReadBuffer});
    return handle;
}

RgResourceHandle PassBuilder::WriteBuffer(RgResourceHandle handle) {
    _impl->Pass->Usages.push_back(ResourceUsage{handle, ResourceAccess::WriteBuffer});
    return handle;
}

void PassBuilder::SetColorAttachment(RgResourceHandle handle) {
    _impl->Pass->Usages.push_back(ResourceUsage{handle, ResourceAccess::ColorAttachment});
}

void PassBuilder::SetPresentTarget(RgResourceHandle handle) {
    _impl->Pass->Usages.push_back(ResourceUsage{handle, ResourceAccess::Present});
}

void PassBuilder::SetExecute(RenderGraphExecuteFn fn) {
    _impl->Pass->Execute = std::move(fn);
}

class RenderGraph::Impl {
public:
    render::Device* Device{nullptr};
    PersistentResourceRegistry* PersistentResources{nullptr};
    vector<ResourceRecord> Resources{};
    vector<PassRecord> Passes{};
    vector<RgPassHandle> CompiledOrder{};
    bool IsCompiled{false};

    ResourceRecord* ResolveResource(RgResourceHandle handle) noexcept {
        if (!handle.IsValid()) {
            return nullptr;
        }
        const size_t index = static_cast<size_t>(handle.Id - 1);
        if (index >= Resources.size()) {
            return nullptr;
        }
        return &Resources[index];
    }

    const ResourceRecord* ResolveResource(RgResourceHandle handle) const noexcept {
        if (!handle.IsValid()) {
            return nullptr;
        }
        const size_t index = static_cast<size_t>(handle.Id - 1);
        if (index >= Resources.size()) {
            return nullptr;
        }
        return &Resources[index];
    }
};

RenderGraph::RenderGraph(render::Device* device, PersistentResourceRegistry* persistentResources) noexcept
    : _impl(make_unique<Impl>()) {
    this->Reset(device, persistentResources);
}

RenderGraph::~RenderGraph() noexcept = default;

void RenderGraph::Reset(render::Device* device, PersistentResourceRegistry* persistentResources) noexcept {
    _impl->Device = device;
    _impl->PersistentResources = persistentResources;
    _impl->Resources.clear();
    _impl->Passes.clear();
    _impl->CompiledOrder.clear();
    _impl->IsCompiled = false;
}

RgResourceHandle RenderGraph::ImportTexture(std::string_view name, const ImportedTextureDesc& desc) {
    ResourceRecord record{};
    record.Name = string{name};
    record.Lifetime = RgResourceLifetime::External;
    record.Kind = RgResourceKind::Texture;
    record.ImportedTexture = desc;
    record.TextureDesc = desc.Desc;
    record.InitialTextureState = desc.InitialState;
    record.FinalTextureState = desc.InitialState;
    _impl->Resources.push_back(std::move(record));
    _impl->IsCompiled = false;
    return RgResourceHandle{static_cast<uint32_t>(_impl->Resources.size())};
}

RgResourceHandle RenderGraph::ImportBuffer(std::string_view name, const ImportedBufferDesc& desc) {
    ResourceRecord record{};
    record.Name = string{name};
    record.Lifetime = RgResourceLifetime::External;
    record.Kind = RgResourceKind::Buffer;
    record.ImportedBuffer = desc;
    record.BufferDesc = desc.Desc;
    record.InitialBufferState = desc.InitialState;
    record.FinalBufferState = desc.InitialState;
    _impl->Resources.push_back(std::move(record));
    _impl->IsCompiled = false;
    return RgResourceHandle{static_cast<uint32_t>(_impl->Resources.size())};
}

RgResourceHandle RenderGraph::CreateTransientTexture(std::string_view name, const render::TextureDescriptor& desc) {
    ResourceRecord record{};
    record.Name = string{name};
    record.Lifetime = RgResourceLifetime::Transient;
    record.Kind = RgResourceKind::Texture;
    record.TextureDesc = desc;
    record.InitialTextureState = render::TextureState::Undefined;
    record.FinalTextureState = render::TextureState::Undefined;
    _impl->Resources.push_back(std::move(record));
    _impl->IsCompiled = false;
    return RgResourceHandle{static_cast<uint32_t>(_impl->Resources.size())};
}

RgResourceHandle RenderGraph::CreateTransientBuffer(std::string_view name, const render::BufferDescriptor& desc) {
    ResourceRecord record{};
    record.Name = string{name};
    record.Lifetime = RgResourceLifetime::Transient;
    record.Kind = RgResourceKind::Buffer;
    record.BufferDesc = desc;
    record.InitialBufferState = render::BufferState::Undefined;
    record.FinalBufferState = render::BufferState::Undefined;
    _impl->Resources.push_back(std::move(record));
    _impl->IsCompiled = false;
    return RgResourceHandle{static_cast<uint32_t>(_impl->Resources.size())};
}

RgResourceHandle RenderGraph::ImportPersistentTexture(
    std::string_view name,
    PersistentTextureHandle handle,
    render::TextureState initialState) {
    ResourceRecord record{};
    record.Name = string{name};
    record.Lifetime = RgResourceLifetime::Persistent;
    record.Kind = RgResourceKind::Texture;
    record.PersistentTexture = handle;
    record.InitialTextureState = initialState;
    record.FinalTextureState = initialState;
    if (_impl->PersistentResources != nullptr) {
        auto texture = _impl->PersistentResources->ResolveTexture(handle);
        if (texture.HasValue()) {
            record.TextureDesc = texture.Get()->Desc;
        }
    }
    _impl->Resources.push_back(std::move(record));
    _impl->IsCompiled = false;
    return RgResourceHandle{static_cast<uint32_t>(_impl->Resources.size())};
}

RgPassHandle RenderGraph::AddRasterPass(std::string_view name, const std::function<void(PassBuilder&)>& setup) {
    PassRecord pass{};
    pass.Name = string{name};
    pass.Type = RgPassType::Raster;
    _impl->Passes.push_back(std::move(pass));
    PassBuilder::Impl builderImpl{&_impl->Passes.back()};
    PassBuilder builder{&builderImpl};
    setup(builder);
    _impl->IsCompiled = false;
    return RgPassHandle{static_cast<uint32_t>(_impl->Passes.size())};
}

RgPassHandle RenderGraph::AddCopyPass(std::string_view name, const std::function<void(PassBuilder&)>& setup) {
    PassRecord pass{};
    pass.Name = string{name};
    pass.Type = RgPassType::Copy;
    _impl->Passes.push_back(std::move(pass));
    PassBuilder::Impl builderImpl{&_impl->Passes.back()};
    PassBuilder builder{&builderImpl};
    setup(builder);
    _impl->IsCompiled = false;
    return RgPassHandle{static_cast<uint32_t>(_impl->Passes.size())};
}

bool RenderGraph::Compile(Nullable<string*> reason) noexcept {
    auto fail = [&](std::string_view message) noexcept {
        if (reason.HasValue()) {
            *reason.Get() = string{message};
        }
        _impl->CompiledOrder.clear();
        _impl->IsCompiled = false;
        return false;
    };

    for (auto& resource : _impl->Resources) {
        resource.HasProducer = false;
        resource.FirstUse = -1;
        resource.LastUse = -1;
        resource.FinalTextureState = resource.InitialTextureState;
        resource.FinalBufferState = resource.InitialBufferState;
        resource.TransientTexture.reset();
        resource.TransientDefaultView.reset();
        resource.TransientBuffer.reset();
    }

    vector<vector<uint32_t>> edges(_impl->Passes.size());
    vector<uint32_t> indegree(_impl->Passes.size(), 0);

    for (size_t resourceIndex = 0; resourceIndex < _impl->Resources.size(); ++resourceIndex) {
        int32_t lastWriter = -1;
        vector<int32_t> lastReaders{};
        auto& resource = _impl->Resources[resourceIndex];
        for (size_t passIndex = 0; passIndex < _impl->Passes.size(); ++passIndex) {
            const auto& pass = _impl->Passes[passIndex];
            for (const auto& usage : pass.Usages) {
                if (usage.Resource.Id != resourceIndex + 1) {
                    continue;
                }
                if (resource.FirstUse < 0) {
                    resource.FirstUse = static_cast<int32_t>(passIndex);
                }
                resource.LastUse = static_cast<int32_t>(passIndex);
                if (IsTextureAccess(usage.Access) && resource.Kind != RgResourceKind::Texture) {
                    return fail("texture access references a non-texture render graph resource");
                }
                if (IsBufferAccess(usage.Access) && resource.Kind != RgResourceKind::Buffer) {
                    return fail("buffer access references a non-buffer render graph resource");
                }

                const bool isWrite = IsWriteAccess(usage.Access);
                if (!isWrite && lastWriter < 0 && resource.Lifetime == RgResourceLifetime::Transient) {
                    return fail("transient render graph resource is read before any producer pass");
                }

                if (!isWrite) {
                    if (lastWriter >= 0) {
                        edges[lastWriter].push_back(static_cast<uint32_t>(passIndex));
                    }
                    lastReaders.push_back(static_cast<int32_t>(passIndex));
                } else {
                    resource.HasProducer = true;
                    if (lastWriter >= 0) {
                        edges[lastWriter].push_back(static_cast<uint32_t>(passIndex));
                    }
                    for (const int32_t reader : lastReaders) {
                        edges[reader].push_back(static_cast<uint32_t>(passIndex));
                    }
                    lastReaders.clear();
                    lastWriter = static_cast<int32_t>(passIndex);
                }

                if (IsTextureAccess(usage.Access)) {
                    resource.FinalTextureState = GetRequiredTextureState(pass.Type, usage.Access);
                }
                if (IsBufferAccess(usage.Access)) {
                    resource.FinalBufferState = GetRequiredBufferState(pass.Type, usage.Access);
                }
            }
        }
    }

    for (auto& adjacency : edges) {
        std::sort(adjacency.begin(), adjacency.end());
        adjacency.erase(std::unique(adjacency.begin(), adjacency.end()), adjacency.end());
        for (const uint32_t to : adjacency) {
            indegree[to]++;
        }
    }

    std::queue<uint32_t> ready{};
    for (uint32_t passIndex = 0; passIndex < indegree.size(); ++passIndex) {
        if (indegree[passIndex] == 0) {
            ready.push(passIndex);
        }
    }

    _impl->CompiledOrder.clear();
    while (!ready.empty()) {
        const uint32_t passIndex = ready.front();
        ready.pop();
        _impl->CompiledOrder.push_back(RgPassHandle{passIndex + 1});
        for (const uint32_t to : edges[passIndex]) {
            indegree[to]--;
            if (indegree[to] == 0) {
                ready.push(to);
            }
        }
    }

    if (_impl->CompiledOrder.size() != _impl->Passes.size()) {
        return fail("render graph contains a cycle or unresolved dependency");
    }

    for (auto& resource : _impl->Resources) {
        if (resource.Lifetime != RgResourceLifetime::Transient || _impl->Device == nullptr) {
            continue;
        }
        if (resource.Kind == RgResourceKind::Texture) {
            auto textureOpt = _impl->Device->CreateTexture(resource.TextureDesc);
            if (!textureOpt.HasValue()) {
                return fail("failed to allocate transient render graph texture");
            }
            auto texture = textureOpt.Release();
            texture->SetDebugName(resource.Name);
            resource.TransientTexture = std::move(texture);

            render::TextureViewDescriptor viewDesc{};
            viewDesc.Target = resource.TransientTexture.get();
            viewDesc.Dim = resource.TextureDesc.Dim;
            viewDesc.Format = resource.TextureDesc.Format;
            viewDesc.Range = render::SubresourceRange{0, resource.TextureDesc.DepthOrArraySize, 0, resource.TextureDesc.MipLevels};
            viewDesc.Usage = static_cast<bool>(resource.TextureDesc.Usage & render::TextureUse::RenderTarget)
                                 ? render::TextureViewUsage::RenderTarget
                                 : render::TextureViewUsage::Resource;
            auto viewOpt = _impl->Device->CreateTextureView(viewDesc);
            if (!viewOpt.HasValue()) {
                return fail("failed to create transient render graph texture view");
            }
            resource.TransientDefaultView = viewOpt.Release();
        } else {
            auto bufferOpt = _impl->Device->CreateBuffer(resource.BufferDesc);
            if (!bufferOpt.HasValue()) {
                return fail("failed to allocate transient render graph buffer");
            }
            auto buffer = bufferOpt.Release();
            buffer->SetDebugName(resource.Name);
            resource.TransientBuffer = std::move(buffer);
        }
    }

    _impl->IsCompiled = true;
    return true;
}

bool RenderGraph::Execute(RenderFrameContext& ctx, Nullable<string*> reason) noexcept {
    if (!_impl->IsCompiled && !this->Compile(reason)) {
        return false;
    }
    if (ctx.CommandBuffer == nullptr) {
        if (reason.HasValue()) {
            *reason.Get() = "render graph execute requires a command buffer";
        }
        return false;
    }

    std::unordered_map<uint32_t, ResolvedResource> resolvedResources{};
    std::unordered_map<uint32_t, render::TextureState> currentTextureStates{};
    std::unordered_map<uint32_t, render::BufferState> currentBufferStates{};

    for (size_t resourceIndex = 0; resourceIndex < _impl->Resources.size(); ++resourceIndex) {
        const auto& resource = _impl->Resources[resourceIndex];
        ResolvedResource resolved{};
        if (resource.Kind == RgResourceKind::Texture) {
            if (resource.Lifetime == RgResourceLifetime::External) {
                resolved.Texture = resource.ImportedTexture.Texture;
                resolved.TextureView = resource.ImportedTexture.DefaultView;
            } else if (resource.Lifetime == RgResourceLifetime::Transient) {
                resolved.Texture = resource.TransientTexture.get();
                resolved.TextureView = resource.TransientDefaultView.get();
            } else if (_impl->PersistentResources != nullptr) {
                auto persistent = _impl->PersistentResources->ResolveTexture(resource.PersistentTexture);
                if (persistent.HasValue()) {
                    resolved.Texture = persistent.Get()->TextureObject.get();
                    resolved.TextureView = persistent.Get()->DefaultView.get();
                }
            }
            currentTextureStates.emplace(static_cast<uint32_t>(resourceIndex + 1), resource.InitialTextureState);
        } else {
            if (resource.Lifetime == RgResourceLifetime::External) {
                resolved.Buffer = resource.ImportedBuffer.Buffer;
            } else if (resource.Lifetime == RgResourceLifetime::Transient) {
                resolved.Buffer = resource.TransientBuffer.get();
            }
            currentBufferStates.emplace(static_cast<uint32_t>(resourceIndex + 1), resource.InitialBufferState);
        }
        resolvedResources.emplace(static_cast<uint32_t>(resourceIndex + 1), resolved);
    }

    for (const RgPassHandle handle : _impl->CompiledOrder) {
        const auto& pass = _impl->Passes[handle.Id - 1];
        vector<render::ResourceBarrierDescriptor> barriers{};

        for (const auto& usage : pass.Usages) {
            const auto resourceIt = resolvedResources.find(usage.Resource.Id);
            if (resourceIt == resolvedResources.end()) {
                continue;
            }
            if (IsTextureAccess(usage.Access)) {
                render::Texture* texture = resourceIt->second.Texture;
                if (texture == nullptr) {
                    continue;
                }
                render::TextureState& currentState = currentTextureStates[usage.Resource.Id];
                const render::TextureState targetState = GetRequiredTextureState(pass.Type, usage.Access);
                if (currentState != targetState) {
                    barriers.push_back(render::BarrierTextureDescriptor{
                        .Target = texture,
                        .Before = currentState,
                        .After = targetState,
                    });
                    currentState = targetState;
                }
                continue;
            }
            if (IsBufferAccess(usage.Access)) {
                render::Buffer* buffer = resourceIt->second.Buffer;
                if (buffer == nullptr) {
                    continue;
                }
                render::BufferState& currentState = currentBufferStates[usage.Resource.Id];
                const render::BufferState targetState = GetRequiredBufferState(pass.Type, usage.Access);
                if (currentState != targetState) {
                    barriers.push_back(render::BarrierBufferDescriptor{
                        .Target = buffer,
                        .Before = currentState,
                        .After = targetState,
                    });
                    currentState = targetState;
                }
            }
        }

        if (!barriers.empty()) {
            ctx.CommandBuffer->ResourceBarrier(barriers);
        }

        if (pass.Execute) {
            RenderGraphPassContext::Impl passCtxImpl{};
            passCtxImpl.FrameCtx = &ctx;
            passCtxImpl.CommandBuffer = ctx.CommandBuffer;
            passCtxImpl.ResolvedResources = &resolvedResources;
            RenderGraphPassContext passCtx{&passCtxImpl};
            if (!pass.Execute(passCtx, reason)) {
                return false;
            }
        }
    }

    return true;
}

std::span<const RgPassHandle> RenderGraph::GetCompiledPassOrder() const noexcept {
    return _impl->CompiledOrder;
}

std::optional<render::TextureState> RenderGraph::GetCompiledFinalTextureState(RgResourceHandle handle) const noexcept {
    const auto* resource = _impl->ResolveResource(handle);
    if (resource == nullptr || resource->Kind != RgResourceKind::Texture) {
        return std::nullopt;
    }
    return resource->FinalTextureState;
}

std::optional<render::BufferState> RenderGraph::GetCompiledFinalBufferState(RgResourceHandle handle) const noexcept {
    const auto* resource = _impl->ResolveResource(handle);
    if (resource == nullptr || resource->Kind != RgResourceKind::Buffer) {
        return std::nullopt;
    }
    return resource->FinalBufferState;
}

}  // namespace radray::runtime
