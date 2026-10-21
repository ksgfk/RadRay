#include "framework_bridge.h"

#include <cstring>
#include <fstream>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/renderer.h>
#include <radray/shader_compiler/client.h>

namespace radray::example {
struct DescriptorFrameworkBridge::State {
    render::Device* Native;
    render::Device* Device;
    DescriptorProbe& Probe;
    const DescriptorReuseOptions& Options;
    RenderSystem Services;
    vector<vector<byte>> ArtifactBytes;
    vector<ShaderProgram*> Programs;
    vector<MaterialTemplateDescriptor> Definitions;
    vector<vector<TemplateId>> Templates;
    vector<MaterialId> Materials;
    InstanceParameterLayout InstanceLayout;
    Nullable<render::TextureView*> Image;
    std::optional<ResourceId> Texture;
    unique_ptr<Renderer> Framework;
    std::optional<FrameBuilder> Frame;
    vector<FramePassId> Passes;
    string Failure;
    State(render::Device* native, render::Device* device, DescriptorProbe& probe, const DescriptorReuseOptions& options)
        : Native(native), Device(device), Probe(probe), Options(options), Services(device) {}
};
DescriptorFrameworkBridge::DescriptorFrameworkBridge(render::Device* native, render::Device* probeDevice, DescriptorProbe& probe, const DescriptorReuseOptions& options)
    : _state(make_unique<State>(native, probeDevice, probe, options)) {}
DescriptorFrameworkBridge::~DescriptorFrameworkBridge() = default;
std::string_view DescriptorFrameworkBridge::ImplementationName() noexcept { return "renderer"; }
const string& DescriptorFrameworkBridge::Error() const noexcept { return _state->Failure; }

bool DescriptorFrameworkBridge::Initialize(render::TextureView* image) {
    auto& state = *_state;
    const auto& options = state.Options;
    if (!state.Services.OnInitialize()) return false;
    const vector<std::filesystem::path> includes{options.ShaderRoot / "shaderlib"};
    shader_compiler::Client compiler{options.Compiler.string()};
    state.Image = image;
    for (uint32_t p = 0; p < options.PassCount(); ++p) {
        const bool depth = options.HasDepth() && p == 0;
        const auto sourceName = depth ? "depth.hlsl" : "reuse.hlsl";
        std::ifstream sourceFile{options.ShaderRoot / sourceName, std::ios::binary};
        const string source{std::istreambuf_iterator<char>{sourceFile}, {}};
        if (source.empty()) {
            state.Failure = "Shader source missing";
            return false;
        }
        shader::SourceContractRequest discovery;
        discovery.SourceName = sourceName;
        const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
        discovery.RootSource.assign(bytes.begin(), bytes.end());
        discovery.Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(options.Backend == render::RenderBackend::Vulkan ? shader::ShaderTarget::SPIRV : shader::ShaderTarget::DXIL));
        discovery.Defines = {{"MIXED", "0"}};
        auto contract = compiler.DiscoverSourceContract(discovery, includes);
        if (!contract.Succeeded()) {
            state.Failure = "Shader discovery failed";
            for (const auto& diagnostic : contract.Diagnostics) state.Failure += "\n" + diagnostic.Message;
            return false;
        }
        shader::CompileVariantRequest request;
        request.SourceName = discovery.SourceName;
        request.RootSource = discovery.RootSource;
        request.Targets = discovery.Targets;
        request.Defines = discovery.Defines;
        request.ExpectedContract = contract.Contract.Hash;
        auto compiled = compiler.CompileVariant(request, includes);
        if (compiled.Status != shader::CompileStatus::Success || compiled.Lanes.size() != 1) {
            state.Failure = "Shader compilation failed";
            for (const auto& diagnostic : compiled.Diagnostics) state.Failure += "\n" + diagnostic.Message;
            return false;
        }
        state.ArtifactBytes.push_back(std::move(compiled.Lanes[0].Metadata));
        const auto& metadata = state.ArtifactBytes.back();
        if (!options.Artifacts.empty()) {
            std::error_code error;
            std::filesystem::create_directories(options.Artifacts, error);
            std::ofstream output{options.Artifacts / fmt::format("pass-{}.artifact", p), std::ios::binary};
            output.write(reinterpret_cast<const char*>(metadata.data()), static_cast<std::streamsize>(metadata.size()));
            if (!output) {
                state.Failure = "Artifact save failed";
                return false;
            }
        }
        shader::WireMetadataEnvelope envelope{};
        if (metadata.size() < sizeof(envelope)) return false;
        std::memcpy(&envelope, metadata.data(), sizeof(envelope));
        auto artifact = render::CreateBackendShaderArtifact(*state.Native, metadata,
                                                            {options.Backend == render::RenderBackend::Vulkan ? shader::ShaderTarget::SPIRV : shader::ShaderTarget::DXIL, envelope.GpuArtifact});
        if (!artifact) {
            state.Failure = "Native shader artifact failed";
            return false;
        }
        auto program = ShaderProgram::Create(state.Device, std::move(*artifact));
        if (!program) {
            state.Failure = "Shader program creation failed";
            return false;
        }
        auto owned = state.Services.AdoptShaderProgram(program.Release());
        if (!owned) return false;
        state.Programs.push_back(owned.Get());
        MaterialTemplateDescriptor pass;
        pass.Name = depth ? "Depth" : "Color";
        pass.RegisteredProgram = state.Programs.back()->GetId();
        pass.Primitive.Cull = render::CullMode::None;
        if (p == 1) pass.Primitive.FaceClockwise = render::FrontFace::CW;
        pass.PushConstant = "DrawData";
        pass.Inputs = {{"Objects", MaterialInputSource::SceneObjects}, {"InstanceTint", MaterialInputSource::InstanceParameters}, {"ViewData", MaterialInputSource::ViewConstants}, {"VisibleSlots", MaterialInputSource::VisibleInstanceSlots}};
        if (options.HasDepth()) {
            pass.DepthStencil = render::DepthStencilState::Default();
            pass.DepthStencil->DepthCompare = render::CompareFunction::LessEqual;
        }
        if (!depth) {
            pass.Inputs.push_back({"Tint", MaterialInputSource::MaterialConstants});
            pass.Inputs.push_back({"Image", MaterialInputSource::Texture});
            pass.Inputs.push_back({"ImageSampler", MaterialInputSource::Sampler});
            const auto layout = ReflectInstanceParameterLayout(state.Programs.back()->GetArtifact().Generic(), "Tint", "InstanceTint");
            if (!layout) {
                state.Failure = "Instance schema reflection failed";
                return false;
            }
            state.InstanceLayout = *layout;
        }
        state.Definitions.push_back(std::move(pass));
    }
    for (auto& definition : state.Definitions) definition.ParameterLayouts = {state.InstanceLayout};
    return true;
}

bool DescriptorFrameworkBridge::Prepare(AppFrameContext& frame, uint32_t viewCount, uint32_t sequence) {
    auto& state = *_state;
    const auto& options = state.Options;
    const auto fail = [&]() {
        state.Failure = state.Framework->GetLastError();
        return false;
    };
    if (!state.Framework) {
        state.Framework = make_unique<Renderer>(*frame.GetGpuSystem(), state.Services);
        state.Texture = state.Framework->ImportTexture(state.Image->GetDesc().Target);
        if (!state.Texture) return fail();
        const auto image = state.Framework->CreateTextureView(*state.Texture, state.Image->GetDesc());
        if (!image) return fail();
        for (uint32_t m = 0; m < options.Materials; ++m) {
            vector<TemplateId> templates;
            if (m == 0 || options.IndependentDefinitions()) {
                for (const auto& definition : state.Definitions) {
                    const auto id = state.Framework->RegisterTemplate(definition);
                    if (!id) return fail();
                    templates.push_back(*id);
                }
            } else
                templates = state.Templates.front();
            state.Templates.push_back(templates);
            RendererMaterialDescriptor material;
            material.Templates = templates;
            const auto tint = DescriptorMaterialColor(options, m, 0);
            const auto bytes = std::as_bytes(std::span{tint});
            material.Constants = {vector<byte>{bytes.begin(), bytes.end()}};
            material.Textures = {*image};
            material.Samplers = {{render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge, render::AddressMode::ClampToEdge,
                                  m % 2 ? render::FilterMode::Linear : render::FilterMode::Nearest,
                                  m % 2 ? render::FilterMode::Linear : render::FilterMode::Nearest, render::FilterMode::Nearest, 0, 1}};
            const auto registered = state.Framework->RegisterMaterial(material);
            if (!registered) return fail();
            state.Materials.push_back(*registered);
        }
    }
    state.Frame = state.Framework->BeginFrame(frame);
    if (!state.Frame) return fail();
    const auto count = options.ObjectCount();
    vector<Eigen::Matrix4f> matrices(count, Eigen::Matrix4f::Identity());
    vector<array<float, 4>> instances;
    for (uint32_t i = 0; i < count; ++i) {
        matrices[i](2, 3) = float(i % 5) * .001f;
        instances.push_back(DescriptorInstanceColor(i));
    }
    const auto objects = state.Frame->StoreData(std::as_bytes(std::span{matrices}), 64);
    const auto instanceData = state.Frame->StoreData(std::as_bytes(std::span{instances}), 16);
    if (!objects || !instanceData) return fail();
    state.Passes.clear();
    for (uint32_t v = 0; v < viewCount; ++v) {
        for (uint32_t p = 0; p < options.PassCount(); ++p) {
            const bool depth = options.HasDepth() && p == 0;
            const auto view = DescriptorViewValues(v);
            const auto viewData = state.Frame->StoreData(std::as_bytes(std::span{view}));
            if (!viewData) return fail();
            state.Probe.ObserveData(fmt::format("view:{}", v), std::get<render::ShaderBufferBinding>(*state.Framework->GetResourceValue(*viewData)));
            vector<uint32_t> slots;
            for (uint32_t i = 0; i < count; ++i) slots.push_back(DescriptorVisibleSlot(options, p, i));
            const auto visible = state.Frame->StoreData(std::as_bytes(std::span{slots}), 4);
            if (!visible) return fail();
            state.Probe.ObserveData(fmt::format("visible:{}:{}", v, options.Scene == "visible-order" ? p : 0),
                                    std::get<render::ShaderBufferBinding>(*state.Framework->GetResourceValue(*visible)));
            RendererViewDescriptor viewRequest;
            viewRequest.ConstantBuffers = {*viewData};
            viewRequest.VisibleBuffer = visible;
            const auto viewId = state.Frame->AddView(std::move(viewRequest));
            if (!viewId) return fail();
            RenderOutputInfo output{256, 256, 1, {}, options.HasDepth() ? std::optional{render::TextureFormat::D32_FLOAT} : std::nullopt};
            if (!depth) output.Colors = {render::TextureFormat::RGBA8_UNORM};
            const auto passId = state.Frame->AddPass({*viewId, state.Templates.front()[p], std::move(output)});
            if (!passId) return fail();
            state.Passes.push_back(*passId);
            for (uint32_t object = 0; object < count; ++object) {
                const auto m = object % options.Materials;
                RendererDrawValues values;
                if (options.Scene == "equal-override" || (options.Scene == "changed-override" && m == 0)) {
                    const auto tint = DescriptorMaterialColor(options, m, sequence);
                    const auto bytes = std::as_bytes(std::span{tint});
                    values.Constants = {vector<byte>{bytes.begin(), bytes.end()}};
                }
                values.Objects = objects;
                values.Parameters = {{state.InstanceLayout, *instanceData}};
                RendererDrawDescriptor draw;
                draw.Count = 3;
                draw.Viewport = {0, 0, 256, 256, 0, 1};
                draw.Scissor = DescriptorObjectRect(options, object);
                draw.Push.resize(4);
                std::memcpy(draw.Push.data(), &object, sizeof(object));
                if (!state.Frame->AddDraw(*passId, state.Materials[m], std::move(draw), std::move(values))) return fail();
            }
        }
    }
    return state.Frame->Compile() || fail();
}
void DescriptorFrameworkBridge::Record(render::GraphicsCommandEncoder* encoder, uint32_t view, uint32_t pass) {
    if (!_state->Frame->RecordPass(encoder, _state->Passes[size_t(view) * _state->Options.PassCount() + pass]))
        RADRAY_ABORT("Compiled descriptor sample recording failed");
}
void DescriptorFrameworkBridge::Unregister() {
    auto& state = *_state;
    if (!state.Framework) return;
    state.Framework->ProcessCompletions();
    for (const auto material : state.Materials) state.Framework->UnregisterMaterial(material);
    for (size_t i = 0; i < state.Templates.size(); ++i)
        if (i == 0 || state.Options.IndependentDefinitions())
            for (const auto definition : state.Templates[i]) state.Framework->UnregisterTemplate(definition);
    if (state.Texture) state.Framework->RequestResourceExit(*state.Texture);
    state.Framework->TrimIdleBindings();
    if (state.Texture) {
        const auto completed = state.Framework->TakeResourceExitConfirmations();
        if (std::find(completed.begin(), completed.end(), *state.Texture) == completed.end()) RADRAY_ABORT("Descriptor sample external texture exit was not confirmed");
    }
    state.Frame.reset();
    state.Passes.clear();
    state.Materials.clear();
    state.Templates.clear();
    if (!state.Framework->Shutdown()) RADRAY_ABORT("Descriptor sample has an unfinished framework frame");
    state.Framework.reset();
}
}  // namespace radray::example
