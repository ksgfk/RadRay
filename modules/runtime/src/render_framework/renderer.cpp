#include <radray/runtime/render_framework/renderer.h>

#include <mutex>
#include <algorithm>
#include <utility>
#include <radray/logger.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

namespace radray {
struct Renderer::State {
    enum class Phase : uint8_t { Idle,
                                 Collecting,
                                 Compiled,
                                 Failed };
    struct Request {
        render::PipelineLayout* Layout;
        uint32_t Group;
        vector<ParameterWrite> Writes;
        vector<ResourceId> Dependencies;
        std::optional<DescriptorBlockId> Block;
    };
    struct Template {
        CompiledMaterialTemplate Compiled;
        uint32_t Registrations{1}, Materials{0}, Frames{0};
    };
    struct Material {
        vector<TemplateId> Templates;
        vector<ResourceId> Constants, Textures, Samplers, Resources;
        uint32_t Frames{0};
        bool Registered{true};
    };
    template <class T>
    struct Slot {
        uint32_t Generation{1};
        unique_ptr<T> Value;
    };
    struct View {
        RendererViewDescriptor Descriptor;
        vector<ResourceId> Constants;
        std::optional<ResourceId> Visible;
    };
    struct Pass {
        RendererPassDescriptor Descriptor;
        vector<ResourceId> Constants;
        vector<FrameDrawId> Draws;
    };
    struct Draw {
        FramePassId Pass;
        MaterialId Material;
        RendererDrawDescriptor Descriptor;
        RendererDrawValues Values;
        vector<FrameBindingId> Groups;
        vector<vector<render::ShaderParameterDynamicOffset>> Offsets;
        Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
        vector<render::VertexBufferBinding> Vertices;
        std::optional<render::IndexBufferView> Indices;
        render::BindingHandle Push;
    };
    struct Frame {
        Frame() = default;
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        Frame(Frame&&) = default;
        Frame& operator=(Frame&&) = default;
        uint32_t Generation{1};
        uint64_t Serial{0};
        Phase Status{Phase::Idle};
        bool Preparing{false};
        vector<unique_ptr<FramePreparation>> Preparations;
        vector<Request> Requests;
        vector<View> Views;
        vector<Pass> Passes;
        vector<Draw> Draws;
        unordered_set<ResourceId, RendererHandleHash<RendererResourceTag>> Resources;
        unordered_set<DescriptorBlockId, RendererHandleHash<RendererDescriptorTag>> Bindings;
        unordered_set<TemplateId, RendererHandleHash<RendererTemplateTag>> Templates;
        unordered_set<MaterialId, RendererHandleHash<RendererMaterialTag>> Materials;
    };
    GpuSystem& Host;
    RenderSystem& Services;
    Guid Owner{Guid::NewGuid()};
    RendererResources Resources;
    BindingRegistry Bindings;
    vector<Slot<Template>> Templates;
    vector<Slot<Material>> Materials;
    vector<Frame> Frames;
    unique_ptr<RendererScenes> Scenes;
    std::mutex CompletionMutex;
    vector<RendererFrameId> Completions;
    vector<ResourceId> ResourceExits;
    unordered_set<ResourceId, RendererHandleHash<RendererResourceTag>> RequestedResourceExits;
    string Error;
    bool Stopped{false};
    State(GpuSystem& host, RenderSystem& services) : Host(host), Services(services), Resources(host.GetDevice(), Owner), Bindings(host.GetDevice(), Owner) {}
    Nullable<Frame*> Find(RendererFrameId id) {
        if (id.Owner != Owner || id.Index >= Frames.size() || Frames[id.Index].Generation != id.Generation || Frames[id.Index].Status == Phase::Idle) return nullptr;
        return &Frames[id.Index];
    }
    Nullable<Frame*> FindRecording(RendererFrameId id) {
        const auto frame = Find(id);
        return frame && Host.IsRecordingFrame(id.Index, frame->Serial) ? frame : nullptr;
    }
    Nullable<Template*> Find(TemplateId id) const {
        if (id.Owner != Owner || id.Index >= Templates.size() || Templates[id.Index].Generation != id.Generation) return nullptr;
        return Templates[id.Index].Value.get();
    }
    Nullable<Material*> Find(MaterialId id) const {
        if (id.Owner != Owner || id.Index >= Materials.size() || Materials[id.Index].Generation != id.Generation) return nullptr;
        return Materials[id.Index].Value.get();
    }
    template <class T, class Tag>
    RendererHandle<Tag> Insert(vector<Slot<T>>& slots, unique_ptr<T> value) {
        uint32_t index = 0;
        while (index < slots.size() && (slots[index].Value || slots[index].Generation == UINT32_MAX)) ++index;
        if (index == slots.size()) slots.emplace_back();
        slots[index].Value = std::move(value);
        return {Owner, index, slots[index].Generation};
    }
};
struct Renderer::CompletionLease {
    Nullable<Renderer*> Owner;
    RendererFrameId Id;
    CompletionLease(Renderer* owner, RendererFrameId id) : Owner(owner), Id(id) {}
    CompletionLease(const CompletionLease&) = delete;
    CompletionLease& operator=(const CompletionLease&) = delete;
    CompletionLease(CompletionLease&& other) noexcept : Owner(std::exchange(other.Owner, nullptr)), Id(other.Id) {}
    ~CompletionLease() noexcept {
        if (Owner) Owner->NotifyCompletion(Id);
    }
};
Renderer::Renderer(GpuSystem& host, RenderSystem& services) : _state(make_unique<State>(host, services)) {
    _state->Scenes = make_unique<RendererScenes>(*this, host.GetFlightDataCount());
}
Renderer::~Renderer() noexcept {
    if (!Shutdown()) RADRAY_ABORT("Renderer destruction requires host completion and ended command epochs");
}
const string& Renderer::GetLastError() const noexcept { return _state->Error; }
RendererScenes& Renderer::GetScenes() noexcept { return *_state->Scenes; }
RenderSystem& Renderer::GetServices() const noexcept { return _state->Services; }
render::Device* Renderer::GetDevice() const noexcept { return _state->Host.GetDevice(); }
bool Renderer::IsMaterialRetired(MaterialId id) const noexcept { return !_state->Find(id); }
bool Renderer::IsResourceRetired(ResourceId id) const noexcept { return !_state->Resources.IsValid(id, true); }
std::optional<TemplateId> Renderer::RegisterTemplate(const MaterialTemplateDescriptor& descriptor) {
    auto& state = *_state;
    if (state.Stopped) return std::nullopt;
    auto compiled = CompiledMaterialTemplate::Compile(state.Host.GetDevice(), state.Services, descriptor, state.Error);
    if (!compiled) return std::nullopt;
    for (uint32_t i = 0; i < state.Templates.size(); ++i) {
        auto& entry = state.Templates[i].Value;
        if (entry && entry->Registrations && entry->Compiled.Equivalent(*compiled)) {
            if (entry->Registrations == UINT32_MAX) return std::nullopt;
            ++entry->Registrations;
            return TemplateId{state.Owner, i, state.Templates[i].Generation};
        }
    }
    return state.Insert<State::Template, RendererTemplateTag>(state.Templates, make_unique<State::Template>(State::Template{std::move(*compiled)}));
}
bool Renderer::UnregisterTemplate(TemplateId id) {
    const auto entry = _state->Find(id);
    if (!entry || !entry->Registrations) return false;
    --entry->Registrations;
    Collect();
    return true;
}
Nullable<const CompiledMaterialTemplate*> Renderer::GetTemplate(TemplateId id) const noexcept {
    const auto entry = _state->Find(id);
    return entry ? &entry->Compiled : nullptr;
}
std::optional<MaterialId> Renderer::RegisterMaterial(const RendererMaterialDescriptor& descriptor) {
    auto& state = *_state;
    if (state.Stopped || descriptor.Templates.empty()) return std::nullopt;
    for (const auto id : descriptor.Templates) {
        const auto entry = state.Find(id);
        if (!entry || !entry->Registrations) return std::nullopt;
    }
    auto material = make_unique<State::Material>();
    material->Templates = descriptor.Templates;
    material->Textures = descriptor.Textures;
    for (const auto id : material->Textures) {
        const auto value = state.Resources.Value(id);
        if (!state.Resources.IsValid(id) || !value || !std::holds_alternative<render::TextureView*>(*value)) return std::nullopt;
    }
    for (const auto& bytes : descriptor.Constants) {
        const auto value = state.Resources.StoreData(bytes);
        if (!value) {
            Collect();
            return std::nullopt;
        }
        material->Constants.push_back(*value);
    }
    for (const auto& sampler : descriptor.Samplers) {
        const auto value = state.Resources.CreateSampler(sampler);
        if (!value) {
            Collect();
            return std::nullopt;
        }
        material->Samplers.push_back(*value);
    }
    for (const auto id : material->Templates) {
        for (const auto& input : state.Find(id)->Compiled.Inputs) {
            if (input.Binding.Immutable) continue;
            const auto end = size_t(input.Source.ValueIndex) + input.Binding.Count;
            if ((input.Source.Source == MaterialInputSource::MaterialConstants && end > material->Constants.size()) ||
                (input.Source.Source == MaterialInputSource::Texture && end > material->Textures.size()) ||
                (input.Source.Source == MaterialInputSource::Sampler && end > material->Samplers.size())) {
                state.Error = "Material defaults do not cover the complete template inputs";
                Collect();
                return std::nullopt;
            }
            if (input.Source.Source == MaterialInputSource::MaterialConstants)
                for (size_t i = input.Source.ValueIndex; i < end; ++i)
                    if (std::get<render::ShaderBufferBinding>(*state.Resources.Value(material->Constants[i])).Range.Size < input.BufferSize) {
                        state.Error = "Material constant bytes are shorter than reflection";
                        Collect();
                        return std::nullopt;
                    }
        }
    }
    for (const auto* list : {&material->Constants, &material->Textures, &material->Samplers})
        for (const auto id : *list)
            if (std::find(material->Resources.begin(), material->Resources.end(), id) == material->Resources.end()) material->Resources.push_back(id);
    size_t retained = 0;
    for (const auto id : material->Resources) {
        if (!state.Resources.RetainRegistration(id)) {
            for (size_t i = 0; i < retained; ++i) state.Resources.ReleaseRegistration(material->Resources[i]);
            Collect();
            return std::nullopt;
        }
        ++retained;
    }
    for (const auto id : material->Templates) ++state.Find(id)->Materials;
    return state.Insert<State::Material, RendererMaterialTag>(state.Materials, std::move(material));
}
std::optional<MaterialId> Renderer::ReplaceMaterial(MaterialId old, const RendererMaterialDescriptor& descriptor) {
    if (!IsMaterialValid(old)) return std::nullopt;
    const auto replacement = RegisterMaterial(descriptor);
    if (replacement) UnregisterMaterial(old);
    return replacement;
}
bool Renderer::UnregisterMaterial(MaterialId id) {
    const auto entry = _state->Find(id);
    if (!entry || !entry->Registered) return false;
    entry->Registered = false;
    Collect();
    return true;
}
bool Renderer::IsMaterialValid(MaterialId id) const noexcept {
    const auto entry = _state->Find(id);
    return entry && entry->Registered;
}
std::span<const TemplateId> Renderer::GetMaterialTemplates(MaterialId id) const noexcept {
    const auto entry = _state->Find(id);
    return entry ? std::span<const TemplateId>{entry->Templates} : std::span<const TemplateId>{};
}
std::optional<FrameBuilder> Renderer::BeginFrame(AppFrameContext& frame) {
    ProcessCompletions();
    auto& state = *_state;
    if (state.Stopped || frame.GetGpuSystem() != &state.Host || !frame.IsRecording()) {
        state.Error = "Renderer requires an open frame from its attached host";
        return std::nullopt;
    }
    if (frame.FlightIndex() >= state.Frames.size()) state.Frames.resize(size_t(frame.FlightIndex()) + 1);
    auto& slot = state.Frames[frame.FlightIndex()];
    if (slot.Status != State::Phase::Idle || slot.Generation == UINT32_MAX) {
        state.Error = "Renderer flight is still admitted or generation is exhausted";
        return std::nullopt;
    }
    slot.Serial = frame.FrameSerial();
    slot.Status = State::Phase::Collecting;
    const RendererFrameId id{state.Owner, frame.FlightIndex(), slot.Generation};
    // GpuSystem releases this lease only after its real queue completion and command epoch retirement.
    frame.RetainGpuForFrameRT(CompletionLease{this, id});
    return FrameBuilder{this, id};
}
void Renderer::NotifyCompletion(RendererFrameId id) {
    std::lock_guard lock{_state->CompletionMutex};
    _state->Completions.push_back(id);
}
void Renderer::ProcessCompletions() {
    auto& state = *_state;
    vector<RendererFrameId> complete;
    {
        std::lock_guard lock{state.CompletionMutex};
        complete.swap(state.Completions);
    }
    for (const auto id : complete) {
        const auto frame = state.Find(id);
        if (!frame) RADRAY_ABORT("Renderer received an invalid host completion lease");
        for (const auto block : frame->Bindings) state.Bindings.Release(block);
        for (const auto resource : frame->Resources) state.Resources.ReleaseFrame(resource);
        for (const auto material : frame->Materials) --state.Find(material)->Frames;
        for (const auto definition : frame->Templates) --state.Find(definition)->Frames;
        frame->Bindings.clear();
        frame->Resources.clear();
        frame->Requests.clear();
        frame->Views.clear();
        frame->Passes.clear();
        frame->Draws.clear();
        frame->Preparations.clear();
        frame->Preparing = false;
        frame->Materials.clear();
        frame->Templates.clear();
        frame->Serial = 0;
        frame->Status = State::Phase::Idle;
        ++frame->Generation;
    }
    Collect();
    state.Scenes->PollExits();
}
void Renderer::Collect() {
    for (auto& slot : _state->Materials) {
        const auto& entry = slot.Value;
        if (!entry || entry->Registered || entry->Frames) continue;
        for (const auto id : entry->Resources) _state->Resources.ReleaseRegistration(id);
        for (const auto id : entry->Templates) --_state->Find(id)->Materials;
        slot.Value.reset();
        if (slot.Generation != UINT32_MAX) ++slot.Generation;
    }
    for (auto& slot : _state->Templates) {
        if (!slot.Value || slot.Value->Registrations || slot.Value->Materials || slot.Value->Frames) continue;
        slot.Value.reset();
        if (slot.Generation != UINT32_MAX) ++slot.Generation;
    }
    auto completed = _state->Resources.Collect(_state->Bindings);
    for (const auto id : completed)
        if (_state->RequestedResourceExits.erase(id)) _state->ResourceExits.push_back(id);
}
std::optional<ResourceId> Renderer::ImportBuffer(render::Buffer* buffer) { return _state->Stopped ? std::nullopt : _state->Resources.ImportBuffer(buffer); }
std::optional<ResourceId> Renderer::CreateBuffer(const render::BufferDescriptor& descriptor) { return _state->Stopped ? std::nullopt : _state->Resources.CreateBuffer(descriptor); }
Nullable<render::Buffer*> Renderer::GetBuffer(ResourceId id) const noexcept { return _state->Resources.Buffer(id); }
std::optional<ResourceId> Renderer::ImportTexture(render::Texture* texture) { return _state->Stopped ? std::nullopt : _state->Resources.ImportTexture(texture); }
std::optional<ResourceId> Renderer::CreateTextureView(ResourceId texture, render::TextureViewDescriptor descriptor) {
    return _state->Stopped ? std::nullopt : _state->Resources.CreateView(texture, descriptor);
}
std::optional<ResourceId> Renderer::CreateBufferRange(ResourceId buffer, render::BufferRange range, uint32_t stride) {
    return _state->Stopped ? std::nullopt : _state->Resources.CreateBufferRange(buffer, range, stride);
}
bool Renderer::RequestResourceExit(ResourceId id) {
    if (!_state->Resources.RequestExit(id, _state->Bindings)) return false;
    _state->RequestedResourceExits.insert(id);
    Collect();
    return true;
}
bool Renderer::RetireResource(ResourceId id) {
    const auto requested = _state->Resources.RequestExit(id, _state->Bindings);
    Collect();
    return requested;
}
vector<ResourceId> Renderer::TakeResourceExitConfirmations() { return std::exchange(_state->ResourceExits, {}); }
std::optional<render::ShaderParameterValue> Renderer::GetResourceValue(ResourceId id) const { return _state->Resources.Value(id); }
bool Renderer::IsResourceValid(ResourceId id) const noexcept { return _state->Resources.IsValid(id); }
void Renderer::TrimIdleBindings() {
    ProcessCompletions();
    _state->Bindings.TrimIdle(true);
    Collect();
}
void Renderer::SetBindingBudget(BindingCacheBudget budget) {
    _state->Bindings.SetBudget(budget);
    Collect();
}
bool Renderer::Shutdown() {
    ProcessCompletions();
    for (const auto& frame : _state->Frames)
        if (frame.Status != State::Phase::Idle) return false;
    _state->Scenes->Clear();
    for (auto& slot : _state->Materials)
        if (slot.Value) slot.Value->Registered = false;
    for (auto& slot : _state->Templates)
        if (slot.Value) slot.Value->Registrations = 0;
    _state->Resources.RequestAllExits(_state->Bindings);
    _state->Bindings.TrimIdle(true);
    Collect();
    _state->Stopped = true;
    return true;
}
bool Renderer::UseResource(RendererFrameId id, ResourceId resource) {
    const auto frame = _state->FindRecording(id);
    if (!frame || frame->Status != State::Phase::Collecting) return false;
    if (frame->Resources.contains(resource)) return true;
    if (!_state->Resources.RetainFrame(resource)) return false;
    frame->Resources.insert(resource);
    return true;
}
bool Renderer::UseMaterial(RendererFrameId id, MaterialId material) {
    const auto frame = _state->FindRecording(id);
    const auto entry = _state->Find(material);
    if (!frame || frame->Status != State::Phase::Collecting || !entry) return false;
    if (frame->Materials.contains(material)) return true;
    if (!entry->Registered) return false;
    for (const auto resource : entry->Resources)
        if (!UseResource(id, resource)) return false;
    frame->Materials.insert(material);
    ++entry->Frames;
    return true;
}
std::optional<ResourceId> Renderer::StoreData(RendererFrameId frame, std::span<const byte> bytes, uint32_t stride) {
    const auto slot = _state->FindRecording(frame);
    if (!slot || slot->Status != State::Phase::Collecting) return std::nullopt;
    const auto id = _state->Resources.StoreData(bytes, stride);
    return id && UseResource(frame, *id) ? id : std::nullopt;
}
std::optional<FrameBindingId> Renderer::AddBindings(RendererFrameId id, render::PipelineLayout* layout, uint32_t group,
                                                    std::span<const ParameterWrite> writes, std::span<const ResourceId> dependencies) {
    const auto frame = _state->FindRecording(id);
    if (!frame || frame->Status != State::Phase::Collecting) return std::nullopt;
    for (const auto resource : dependencies)
        if (!UseResource(id, resource)) return std::nullopt;
    const auto index = uint32_t(frame->Requests.size());
    frame->Requests.push_back({layout, group, {writes.begin(), writes.end()}, {dependencies.begin(), dependencies.end()}, {}});
    return FrameBindingId{id, index};
}
bool Renderer::Compile(RendererFrameId id) {
    auto& state = *_state;
    const auto frame = state.FindRecording(id);
    if (!frame || frame->Status != State::Phase::Collecting || frame->Preparing) return false;
    frame->Preparing = true;
    FrameBuilder builder{this, id};
    bool prepared = true;
    for (auto& preparation : frame->Preparations) prepared = preparation->Prepare(builder) && prepared;
    frame->Preparing = false;
    if (!prepared || !CompileDraws(id)) {
        if (state.Error.empty()) state.Error = "Frame draw compilation failed";
        frame->Status = State::Phase::Failed;
        return false;
    }
    for (auto& request : frame->Requests) {
        const auto block = state.Bindings.Resolve(request.Layout, request.Group, request.Writes, request.Dependencies);
        if (!block) {
            state.Error = "Complete parameter group compilation failed";
            frame->Status = State::Phase::Failed;
            return false;
        }
        if (!frame->Bindings.contains(*block)) {
            if (!state.Bindings.Retain(*block)) {
                frame->Status = State::Phase::Failed;
                return false;
            }
            frame->Bindings.insert(*block);
        }
        request.Block = block;
    }
    frame->Status = State::Phase::Compiled;
    return true;
}
std::optional<FrameViewId> Renderer::AddView(RendererFrameId id, RendererViewDescriptor descriptor) {
    const auto frame = _state->FindRecording(id);
    if (!frame || frame->Status != State::Phase::Collecting) return std::nullopt;
    for (const auto resource : descriptor.ConstantBuffers)
        if (!UseResource(id, resource)) return std::nullopt;
    if (descriptor.VisibleBuffer && !UseResource(id, *descriptor.VisibleBuffer)) return std::nullopt;
    const auto index = uint32_t(frame->Views.size());
    frame->Views.push_back({std::move(descriptor), {}, {}});
    return FrameViewId{id, index};
}
std::optional<FramePassId> Renderer::AddPass(RendererFrameId id, RendererPassDescriptor descriptor) {
    const auto frame = _state->FindRecording(id);
    const auto definition = _state->Find(descriptor.Template);
    if (!frame || frame->Status != State::Phase::Collecting || descriptor.View.Frame != id || descriptor.View.Index >= frame->Views.size() || !definition ||
        (!definition->Registrations && !definition->Materials)) return std::nullopt;
    for (const auto resource : descriptor.FrameBuffers)
        if (!UseResource(id, resource)) return std::nullopt;
    if (frame->Templates.insert(descriptor.Template).second) ++definition->Frames;
    const auto index = uint32_t(frame->Passes.size());
    frame->Passes.push_back({std::move(descriptor), {}});
    return FramePassId{id, index};
}
std::optional<FrameDrawId> Renderer::AddDraw(FramePassId pass, MaterialId material, RendererDrawDescriptor descriptor, RendererDrawValues values) {
    const auto frame = _state->FindRecording(pass.Frame);
    const auto entry = _state->Find(material);
    if (!frame || frame->Status != State::Phase::Collecting || pass.Index >= frame->Passes.size() || !entry || (!entry->Registered && (!frame->Preparing || !frame->Materials.contains(material)))) return std::nullopt;
    const auto definition = frame->Passes[pass.Index].Descriptor.Template;
    if (std::find(entry->Templates.begin(), entry->Templates.end(), definition) == entry->Templates.end()) return std::nullopt;
    if (!UseMaterial(pass.Frame, material)) return std::nullopt;
    for (const auto* list : {&values.ConstantBuffers, &values.Textures})
        for (const auto resource : *list)
            if (!UseResource(pass.Frame, resource)) return std::nullopt;
    if (values.Objects && !UseResource(pass.Frame, *values.Objects)) return std::nullopt;
    for (const auto& parameter : values.Parameters)
        if (!UseResource(pass.Frame, parameter.Buffer)) return std::nullopt;
    for (const auto& vertex : descriptor.Vertices)
        if (!UseResource(pass.Frame, vertex.Buffer)) return std::nullopt;
    if (descriptor.Indices && !UseResource(pass.Frame, descriptor.Indices->Buffer)) return std::nullopt;
    const auto index = uint32_t(frame->Draws.size());
    frame->Draws.push_back({pass, material, std::move(descriptor), std::move(values)});
    frame->Passes[pass.Index].Draws.push_back({pass.Frame, index});
    return FrameDrawId{pass.Frame, index};
}
bool Renderer::CompileDraws(RendererFrameId id) {
    auto& state = *_state;
    const auto frame = state.FindRecording(id);
    if (!frame) return false;
    const auto constants = [&](const vector<vector<byte>>& bytes, const vector<ResourceId>& buffers, vector<ResourceId>& output) {
        if (!buffers.empty()) {
            output = buffers;
            return true;
        }
        for (const auto& value : bytes) {
            const auto resource = StoreData(id, value, 0);
            if (!resource) return false;
            output.push_back(*resource);
        }
        return true;
    };
    for (auto& view : frame->Views) {
        if (!constants(view.Descriptor.Constants, view.Descriptor.ConstantBuffers, view.Constants)) return false;
        view.Visible = view.Descriptor.VisibleBuffer;
        if (!view.Visible && !view.Descriptor.VisibleSlots.empty()) {
            view.Visible = StoreData(id, std::as_bytes(std::span{view.Descriptor.VisibleSlots}), 4);
            if (!view.Visible) return false;
        }
    }
    for (auto& pass : frame->Passes)
        if (!constants(pass.Descriptor.FrameConstants, pass.Descriptor.FrameBuffers, pass.Constants)) return false;
    unordered_map<size_t, vector<FrameBindingId>> groupRequests;
    unordered_map<size_t, vector<uint32_t>> pipelineRequests;
    for (auto& draw : frame->Draws) {
        const auto& pass = frame->Passes[draw.Pass.Index];
        const auto& view = frame->Views[pass.Descriptor.View.Index];
        const auto& plan = state.Find(pass.Descriptor.Template)->Compiled;
        const auto material = state.Find(draw.Material);
        auto& geometry = draw.Descriptor;
        if (geometry.Push.empty()) geometry.Push = plan.Descriptor.ScenePushValues;
        if (geometry.Push.size() != plan.PushSize) {
            state.Error = "Draw push bytes differ from the template";
            return false;
        }
        draw.Push = plan.Push;
        vector<ResourceId> overrideConstants, overrideSamplers;
        if (!constants(draw.Values.Constants, draw.Values.ConstantBuffers, overrideConstants)) return false;
        for (const auto& descriptor : draw.Values.Samplers) {
            const auto resource = state.Resources.CreateSampler(descriptor);
            if (!resource || !UseResource(id, *resource)) return false;
            overrideSamplers.push_back(*resource);
        }
        for (const auto& group : plan.Groups) {
            vector<ParameterWrite> writes;
            vector<ResourceId> dependencies;
            vector<render::ShaderParameterDynamicOffset> offsets;
            for (const auto inputIndex : group.Inputs) {
                const auto& input = plan.Inputs[inputIndex];
                for (uint32_t element = 0; element < input.Binding.Count; ++element) {
                    const auto index = input.Source.ValueIndex + element;
                    std::optional<ResourceId> resource;
                    const auto select = [&](const vector<ResourceId>& source) { if (index < source.size()) resource = source[index]; };
                    switch (input.Source.Source) {
                        case MaterialInputSource::SceneObjects: resource = draw.Values.Objects; break;
                        case MaterialInputSource::VisibleInstanceSlots: resource = view.Visible; break;
                        case MaterialInputSource::FrameConstants: select(pass.Constants); break;
                        case MaterialInputSource::ViewConstants: select(view.Constants); break;
                        case MaterialInputSource::MaterialConstants: select(overrideConstants.empty() ? material->Constants : overrideConstants); break;
                        case MaterialInputSource::Texture: select(draw.Values.Textures.empty() ? material->Textures : draw.Values.Textures); break;
                        case MaterialInputSource::Sampler: select(overrideSamplers.empty() ? material->Samplers : overrideSamplers); break;
                        case MaterialInputSource::InstanceParameters:
                            if (index < plan.Descriptor.ParameterLayouts.size())
                                for (const auto& parameter : draw.Values.Parameters)
                                    if (parameter.Layout == plan.Descriptor.ParameterLayouts[index]) {
                                        resource = parameter.Buffer;
                                        break;
                                    }
                            break;
                        default: return false;
                    }
                    if (!resource || !UseResource(id, *resource)) {
                        state.Error = fmt::format("Missing frame input '{}'", input.Source.Name);
                        return false;
                    }
                    const auto value = state.Resources.Value(*resource);
                    if (!value) return false;
                    const auto* buffer = std::get_if<render::ShaderBufferBinding>(&*value);
                    if (input.Binding.LogicalKind == shader::ShaderBindingKind::CBuffer && (!buffer || buffer->Range.Size < input.BufferSize)) return false;
                    if (input.Source.Source == MaterialInputSource::SceneObjects && (!buffer || buffer->StructureByteStride != 64)) return false;
                    if (input.Source.Source == MaterialInputSource::VisibleInstanceSlots && (!buffer || buffer->StructureByteStride != 4)) return false;
                    if (input.Source.Source == MaterialInputSource::InstanceParameters && (!buffer || buffer->StructureByteStride != plan.Descriptor.ParameterLayouts[index].Stride)) return false;
                    if (input.Source.Source == MaterialInputSource::Texture && !std::holds_alternative<render::TextureView*>(*value)) return false;
                    if (input.Source.Source == MaterialInputSource::Sampler && !std::holds_alternative<render::Sampler*>(*value)) return false;
                    writes.push_back({input.Handle, element, *value});
                    dependencies.push_back(*resource);
                }
                if (input.Binding.Dynamic) offsets.push_back({input.Handle, 0});
            }
            auto* layout = plan.Program->GetPipelineLayout();
            HashCode hash;
            hash.Add(layout);
            hash.Add(group.Index);
            for (const auto resource : dependencies) {
                hash.Add(resource.Index);
                hash.Add(resource.Generation);
            }
            auto& candidates = groupRequests[hash.ToHashCode()];
            std::optional<FrameBindingId> binding;
            for (const auto candidate : candidates) {
                const auto& request = frame->Requests[candidate.Index];
                if (request.Layout != layout || request.Group != group.Index || request.Dependencies != dependencies || request.Writes.size() != writes.size()) continue;
                if (std::equal(writes.begin(), writes.end(), request.Writes.begin(), [](const auto& a, const auto& b) {
                        return a.Binding == b.Binding && a.Element == b.Element;
                    })) {
                    binding = candidate;
                    break;
                }
            }
            // Resource versions are immutable; matching ordered resources and declarations imply identical writes.
            if (!binding) {
                binding = AddBindings(id, layout, group.Index, writes, dependencies);
                if (binding) candidates.push_back(*binding);
            }
            if (!binding) return false;
            draw.Groups.push_back(*binding);
            draw.Offsets.push_back(std::move(offsets));
        }
        const auto& output = pass.Descriptor.Output;
        if (!output.Samples || (plan.Descriptor.DepthStencil && !output.Depth)) return false;
        HashCode pipelineHash;
        pipelineHash.Add(pass.Descriptor.Template.Index);
        pipelineHash.Add(pass.Descriptor.Template.Generation);
        pipelineHash.Add(geometry.Topology);
        pipelineHash.Add(geometry.ReverseCulling);
        pipelineHash.Add(pass.Descriptor.DefaultDepthTest);
        pipelineHash.Add(output.Samples);
        pipelineHash.Add(output.Depth.value_or(render::TextureFormat::UNKNOWN));
        for (const auto format : output.Colors) pipelineHash.Add(format);
        auto& pipelineCandidates = pipelineRequests[pipelineHash.ToHashCode()];
        for (const auto index : pipelineCandidates) {
            const auto& previous = frame->Draws[index];
            const auto& previousPass = frame->Passes[previous.Pass.Index].Descriptor;
            if (previousPass.Template == pass.Descriptor.Template && previousPass.DefaultDepthTest == pass.Descriptor.DefaultDepthTest &&
                previousPass.Output.Samples == output.Samples && previousPass.Output.Colors == output.Colors && previousPass.Output.Depth == output.Depth &&
                previous.Descriptor.Topology == geometry.Topology && previous.Descriptor.ReverseCulling == geometry.ReverseCulling && previous.Descriptor.Geometry == geometry.Geometry) {
                draw.Pipeline = previous.Pipeline;
                break;
            }
        }
        if (!draw.Pipeline) {
            auto& cache = state.Services.GetGraphicsPipelineCache();
            const auto layout = cache.Match(geometry.Geometry, plan.Program);
            if (!layout) return false;
            auto primitive = plan.Descriptor.Primitive;
            primitive.Topology = geometry.Topology;
            if (geometry.ReverseCulling) primitive.FaceClockwise = primitive.FaceClockwise == render::FrontFace::CW ? render::FrontFace::CCW : render::FrontFace::CW;
            auto depth = plan.Descriptor.DepthStencil;
            if (depth)
                depth->Format = *output.Depth;
            else if (output.Depth) {
                depth = render::DepthStencilState::Default();
                depth->Format = *output.Depth;
                depth->DepthTestEnable = pass.Descriptor.DefaultDepthTest;
                depth->DepthWriteEnable = pass.Descriptor.DefaultDepthTest;
            }
            vector<render::ColorTargetState> colors;
            for (const auto format : output.Colors) colors.push_back({format, plan.Descriptor.Blend, plan.Descriptor.WriteMask});
            auto samples = plan.Descriptor.Samples;
            samples.Count = output.Samples;
            draw.Pipeline = cache.GetOrCreate({plan.Program, *layout.Get(), primitive, depth, samples, colors});
            if (!draw.Pipeline) return false;
            pipelineCandidates.push_back(uint32_t(&draw - frame->Draws.data()));
        }
        for (const auto& vertex : geometry.Vertices) {
            const auto value = state.Resources.Value(vertex.Buffer);
            const auto* buffer = value ? std::get_if<render::ShaderBufferBinding>(&*value) : nullptr;
            if (!buffer || vertex.Offset > buffer->Range.Size || vertex.Size > buffer->Range.Size - vertex.Offset) return false;
            draw.Vertices.push_back({vertex.Binding, {buffer->Target, buffer->Range.Offset + vertex.Offset, vertex.Size}});
        }
        if (geometry.Indices) {
            const auto value = state.Resources.Value(geometry.Indices->Buffer);
            const auto* buffer = value ? std::get_if<render::ShaderBufferBinding>(&*value) : nullptr;
            if (!buffer || geometry.Indices->Offset > buffer->Range.Size || buffer->Range.Offset + geometry.Indices->Offset > UINT32_MAX ||
                (geometry.Indices->Stride != 2 && geometry.Indices->Stride != 4)) return false;
            draw.Indices = {buffer->Target, uint32_t(buffer->Range.Offset + geometry.Indices->Offset), geometry.Indices->Stride};
        }
    }
    return true;
}
bool Renderer::Record(render::GraphicsCommandEncoder* encoder, FrameDrawId id) const {
    return Record(encoder, std::span{&id, 1});
}
bool Renderer::Record(render::GraphicsCommandEncoder* encoder, std::span<const FrameDrawId> draws) const {
    if (draws.empty()) return true;
    const auto frame = _state->FindRecording(draws.front().Frame);
    if (!frame || frame->Status != State::Phase::Compiled) return false;
    for (const auto id : draws)
        if (id.Frame != draws.front().Frame || id.Index >= frame->Draws.size()) return false;
    Nullable<const State::Draw*> previous{nullptr};
    for (const auto id : draws) {
        const auto& draw = frame->Draws[id.Index];
        const auto& geometry = draw.Descriptor;
        if (!geometry.Count || !geometry.Instances || !geometry.Scissor.Width || !geometry.Scissor.Height) continue;
        const auto viewportEqual = [](const Viewport& a, const Viewport& b) {
            return a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height && a.MinDepth == b.MinDepth && a.MaxDepth == b.MaxDepth;
        };
        const auto rectEqual = [](const Rect& a, const Rect& b) { return a.X == b.X && a.Y == b.Y && a.Width == b.Width && a.Height == b.Height; };
        if (!previous || !viewportEqual(previous->Descriptor.Viewport, geometry.Viewport)) encoder->SetViewport(geometry.Viewport);
        if (!previous || !rectEqual(previous->Descriptor.Scissor, geometry.Scissor)) encoder->SetScissor(geometry.Scissor);
        const bool samePipeline = previous && previous->Pipeline.Get() == draw.Pipeline.Get();
        if (!samePipeline) encoder->BindGraphicsPipelineState(draw.Pipeline.Get());
        for (size_t i = 0; i < draw.Groups.size(); ++i) {
            const auto& request = frame->Requests[draw.Groups[i].Index];
            bool bound = false;
            if (samePipeline && i < previous->Groups.size()) {
                const auto& prior = frame->Requests[previous->Groups[i].Index];
                bound = request.Group == prior.Group && request.Block == prior.Block && draw.Offsets[i] == previous->Offsets[i];
            }
            if (!bound && (!request.Block || !_state->Bindings.Bind(encoder, request.Group, *request.Block, draw.Offsets[i]))) return false;
        }
        if (draw.Push.IsValid() && !encoder->SetPushConstants(draw.Push, geometry.Push)) return false;
        const bool sameVertices = samePipeline && std::equal(draw.Vertices.begin(), draw.Vertices.end(), previous->Vertices.begin(), previous->Vertices.end(), [](const auto& a, const auto& b) {
                                      return a.Binding == b.Binding && a.View.Target == b.View.Target && a.View.Offset == b.View.Offset && a.View.Size == b.View.Size;
                                  });
        if (!sameVertices) encoder->BindVertexBuffers(draw.Vertices);
        if (draw.Indices) {
            if (!previous || !previous->Indices || previous->Indices->Target != draw.Indices->Target || previous->Indices->Offset != draw.Indices->Offset || previous->Indices->Stride != draw.Indices->Stride)
                encoder->BindIndexBuffer(*draw.Indices);
            encoder->DrawIndexed(geometry.Count, geometry.Instances, geometry.First, geometry.VertexOffset, geometry.FirstInstance);
        } else
            encoder->Draw(geometry.Count, geometry.Instances, geometry.First, geometry.FirstInstance);
        previous = &draw;
    }
    return true;
}
bool Renderer::RecordPass(render::GraphicsCommandEncoder* encoder, FramePassId pass) const {
    const auto frame = _state->FindRecording(pass.Frame);
    if (!frame || frame->Status != State::Phase::Compiled || pass.Index >= frame->Passes.size()) return false;
    return Record(encoder, frame->Passes[pass.Index].Draws);
}
bool Renderer::Bind(render::GraphicsCommandEncoder* encoder, FrameBindingId id, std::span<const render::ShaderParameterDynamicOffset> offsets) const {
    const auto frame = _state->FindRecording(id.Frame);
    if (!frame || frame->Status != State::Phase::Compiled || id.Index >= frame->Requests.size()) return false;
    const auto& request = frame->Requests[id.Index];
    return request.Block && _state->Bindings.Bind(encoder, request.Group, *request.Block, offsets);
}
FrameBuilder::FrameBuilder(Renderer* renderer, RendererFrameId id) noexcept : _renderer(renderer), _id(id) {}
FrameBuilder::FrameBuilder(FrameBuilder&& other) noexcept : _renderer(std::exchange(other._renderer, nullptr)), _id(other._id) {}
FrameBuilder& FrameBuilder::operator=(FrameBuilder&& other) noexcept {
    if (this != &other) {
        _renderer = std::exchange(other._renderer, nullptr);
        _id = other._id;
    }
    return *this;
}
bool FrameBuilder::IsFor(const AppFrameContext& frame) const noexcept {
    if (!_renderer || frame.GetGpuSystem() != &_renderer->_state->Host || frame.FlightIndex() != _id.Index || !frame.IsRecording()) return false;
    const auto entry = _renderer->_state->FindRecording(_id);
    return entry && entry->Serial == frame.FrameSerial();
}
bool FrameBuilder::IsFor(const Renderer& renderer, uint32_t flight, uint64_t serial) const noexcept {
    if (!_renderer || _renderer.Get() != &renderer || _id.Index != flight) return false;
    const auto entry = _renderer->_state->FindRecording(_id);
    return entry && entry->Serial == serial;
}
bool FrameBuilder::IsCompiled() const noexcept {
    const auto entry = _renderer ? _renderer->_state->FindRecording(_id) : nullptr;
    return entry && entry->Status == Renderer::State::Phase::Compiled;
}
std::optional<FramePreparationId> FrameBuilder::AddPreparation(unique_ptr<FramePreparation> preparation) {
    const auto entry = _renderer ? _renderer->_state->FindRecording(_id) : nullptr;
    if (!entry || !preparation || entry->Status != Renderer::State::Phase::Collecting || entry->Preparing) return std::nullopt;
    const auto index = uint32_t(entry->Preparations.size());
    entry->Preparations.push_back(std::move(preparation));
    return FramePreparationId{_id, index};
}
Nullable<FramePreparation*> FrameBuilder::GetPreparation(FramePreparationId id) const noexcept {
    const auto entry = _renderer ? _renderer->_state->FindRecording(_id) : nullptr;
    return entry && IsCompiled() && id.Frame == _id && id.Index < entry->Preparations.size() ? entry->Preparations[id.Index].get() : nullptr;
}
std::optional<ResourceId> FrameBuilder::StoreData(std::span<const byte> bytes, uint32_t stride) { return _renderer ? _renderer->StoreData(_id, bytes, stride) : std::nullopt; }
bool FrameBuilder::UseResource(ResourceId id) { return _renderer && _renderer->UseResource(_id, id); }
bool FrameBuilder::UseMaterial(MaterialId id) { return _renderer && _renderer->UseMaterial(_id, id); }
std::optional<FrameViewId> FrameBuilder::AddView(RendererViewDescriptor descriptor) { return _renderer ? _renderer->AddView(_id, std::move(descriptor)) : std::nullopt; }
std::optional<FramePassId> FrameBuilder::AddPass(RendererPassDescriptor descriptor) { return _renderer ? _renderer->AddPass(_id, std::move(descriptor)) : std::nullopt; }
std::optional<FrameDrawId> FrameBuilder::AddDraw(FramePassId pass, MaterialId material, RendererDrawDescriptor descriptor, RendererDrawValues values) {
    return _renderer && pass.Frame == _id ? _renderer->AddDraw(pass, material, std::move(descriptor), std::move(values)) : std::nullopt;
}
bool FrameBuilder::Record(render::GraphicsCommandEncoder* encoder, FrameDrawId draw) const { return _renderer && draw.Frame == _id && _renderer->Record(encoder, draw); }
bool FrameBuilder::Record(render::GraphicsCommandEncoder* encoder, std::span<const FrameDrawId> draws) const {
    return _renderer && IsCompiled() && std::all_of(draws.begin(), draws.end(), [&](const auto draw) { return draw.Frame == _id; }) && _renderer->Record(encoder, draws);
}
bool FrameBuilder::RecordPass(render::GraphicsCommandEncoder* encoder, FramePassId pass) const { return _renderer && pass.Frame == _id && _renderer->RecordPass(encoder, pass); }
std::optional<FrameBindingId> FrameBuilder::AddBindings(render::PipelineLayout* layout, uint32_t group, std::span<const ParameterWrite> writes, std::span<const ResourceId> dependencies) {
    return _renderer ? _renderer->AddBindings(_id, layout, group, writes, dependencies) : std::nullopt;
}
bool FrameBuilder::Compile() { return _renderer && _renderer->Compile(_id); }
bool FrameBuilder::Bind(render::GraphicsCommandEncoder* encoder, FrameBindingId id, std::span<const render::ShaderParameterDynamicOffset> offsets) const {
    return _renderer && id.Frame == _id && _renderer->Bind(encoder, id, offsets);
}
}  // namespace radray
