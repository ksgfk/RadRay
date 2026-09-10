#include "imgui_graph_frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <radray/runtime/render_framework/viewport.h>

namespace radray {
namespace {
#include "imgui_shaders.inc"

render::SamplerDescriptor Sampler(int kind) {
    render::SamplerDescriptor sampler;
    sampler.AddressS = sampler.AddressT = sampler.AddressR = render::AddressMode::ClampToEdge;
    sampler.MinFilter = sampler.MagFilter = sampler.MipmapFilter = kind == 2 ? render::FilterMode::Nearest : render::FilterMode::Linear;
    return sampler;
}
MaterialPipelineState UiState(bool blend) {
    MaterialPipelineState state;
    state.Primitive.Cull = render::CullMode::None;
    state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
    if (blend) state.Blend = render::BlendState{{render::BlendFactor::SrcAlpha, render::BlendFactor::OneMinusSrcAlpha, render::BlendOperation::Add},
                                                {render::BlendFactor::One, render::BlendFactor::OneMinusSrcAlpha, render::BlendOperation::Add}};
    return state;
}
const PrimitiveVertexLayout& UiLayout() {
    static const PrimitiveVertexLayout layout{
        {{0, sizeof(ImDrawVert), render::VertexStepMode::Vertex}},
        {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X2},
         {"TEXCOORD", 0, 0, 8, render::VertexFormat::FLOAT32X2},
         {"COLOR", 0, 0, 16, render::VertexFormat::UNORM8X4}}};
    return layout;
}
struct UiConstants {
    array<float, 4> Transform{}, Options{};
};
}  // namespace

bool ImGuiGraph::BuildGraph(RenderGraph& graph, RenderPipelineContext& context, ImGuiGraphFrame frame,
                            std::span<RenderGraphOutputBinding> targets, std::span<const ImGuiSceneOutput> scenes, std::span<const ImGuiGraphImageBinding> images) {
    RADRAY_ASSERT(frame._index == context.FlightIndex());
    auto& self = frame._resources;
    auto& flight = frame._flight;
    flight.FrameSerial = context.FrameSerial();
    if (!flight.Valid) {
        graph.AddDiagnostic("ImGuiSnapshot", "The UI snapshot contains an invalid texture or unsupported callback");
        return false;
    }
    auto& device = self.Device;
    const bool dxil = device.GetBackend() == render::RenderBackend::D3D12;
    if (!self.DrawProgram) self.DrawProgram = self.CreateProgram(
                               dxil ? std::as_bytes(std::span<const unsigned char>{imgui_dxil}) : std::as_bytes(std::span<const unsigned char>{imgui_spirv}), dxil ? imgui_dxil_identity : imgui_spirv_identity);
    if (!self.DrawProgram) {
        graph.AddDiagnostic("ImGuiShader", "Embedded ImGui artifact could not create a shader program");
        self.Error = true;
        return false;
    }
    unordered_map<ImTextureID, RgTextureParameterBinding> bindings;
    unordered_map<ImTextureID, ImGuiColorEncoding> outputEncodings;
    unordered_map<render::Texture*, RgTextureValue> imports;
    const auto import = [&](render::Texture* texture, std::span<render::TextureStates> states, std::span<uint8_t> valid, bool observable) {
        auto found = imports.find(texture);
        if (found != imports.end()) return found->second;
        auto external = make_unique<RenderExternalTexture>(RenderExternalTexture{texture, texture->GetDesc(), states, valid});
        auto handle = graph.ImportTexture(*external, "ImGui.Texture", observable ? RenderGraphExternalAccess::ObservableOutput : RenderGraphExternalAccess::ReadOnly);
        flight.ExternalTextures.push_back(std::move(external));
        imports.emplace(texture, handle);
        return handle;
    };
    for (const auto& request : flight.Requests) {
        if (request.Status == ImTextureStatus_WantDestroy) {
            const auto found = self.GpuTextures.find(request.Id);
            if (found != self.GpuTextures.end()) flight.Retained.push_back(found->second);
            continue;
        }
        auto& texture = self.GpuTextures[request.Id];
        bool full = !texture || texture->Texture->GetDesc().Width != request.Width || texture->Texture->GetDesc().Height != request.Height || texture->Texture->GetDesc().Format != request.Format;
        if (full) {
            render::TextureDescriptor desc{render::TextureDimension::Dim2D, request.Width, request.Height, 1, 1, 1,
                                           request.Format, render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::CopyDestination};
            auto native = device.CreateTexture(desc);
            if (!native) {
                graph.AddDiagnostic("ImGuiTexture", "Dynamic texture allocation failed");
                self.Error = true;
                return false;
            }
            texture = make_shared<UiGpuTexture>();
            texture->Texture = native.Release();
        }
        full |= texture->Valid[0] == 0;
        auto destination = import(texture->Texture.get(), texture->States, texture->Valid, true);
        flight.Retained.push_back(texture);
        vector<ImTextureRect> regions = request.Regions;
        if (full) regions = {{0, 0, uint16_t(request.Width), uint16_t(request.Height)}};
        for (const auto& region : regions) {
            const uint32_t pitch = uint32_t(Align(uint64_t{region.w} * 4, std::max(1u, device.GetDetail().TextureDataPitchAlignment)));
            if (region.w == 0 || region.h == 0 || uint32_t(region.x) + region.w > request.Width || uint32_t(region.y) + region.h > request.Height) {
                graph.AddDiagnostic("ImGuiTexture", "Dynamic texture update exceeds the pixel snapshot");
                return false;
            }
            vector<byte> pixels(uint64_t(pitch) * region.h);
            for (uint32_t row = 0; row < region.h; ++row)
                std::memcpy(pixels.data() + uint64_t(row) * pitch, request.Pixels.data() + (uint64_t(region.y + row) * request.Width + region.x) * 4, uint64_t(region.w) * 4);
            const auto source = graph.UploadBuffer("ImGui.TexturePixels", pixels, render::BufferUse::CopySource);
            destination = graph.NextVersion(destination);
            flight.UploadTickets.push_back(graph.Track(graph.AddCopyBufferToTexturePass("ImGui.TextureUpdate", source, destination,
                                                                                        {0, pitch, 0, 0, region.x, region.y, region.w, region.h})));
        }
        imports.insert_or_assign(texture->Texture.get(), destination);
        bindings.emplace(request.Id, RgTextureParameterBinding{destination});
    }
    flight.AssetStates.reserve(flight.Textures.size());
    flight.AssetValid.reserve(flight.Textures.size());
    for (const auto& [id, record] : flight.Textures) {
        if (record.Graph || bindings.contains(id)) continue;
        RgTextureValue texture;
        if (record.Output.IsValid()) {
            const auto scene = std::find_if(scenes.begin(), scenes.end(), [&](const auto& value) { return value.Output == record.Output; });
            if (scene == scenes.end() || std::count_if(scenes.begin(), scenes.end(), [&](const auto& value) { return value.Output == record.Output; }) != 1) {
                graph.AddDiagnostic("ImGuiOutputImage", "Registered output requires exactly one scene producer in this frame");
                continue;
            }
            texture = scene->Texture;
            outputEncodings.emplace(id, scene->SampleEncoding);
        } else if (record.Dynamic) {
            auto found = self.GpuTextures.find(id);
            if (found == self.GpuTextures.end()) continue;
            const auto& resource = found->second;
            texture = import(resource->Texture.get(), resource->States, resource->Valid, false);
            flight.Retained.push_back(resource);
        } else if (record.Lease) {
            auto& lease = *record.Lease;
            texture = import(lease._texture.get(), lease._states, lease._valid, false);
        } else if (auto native = record.Texture) {
            const auto desc = native->GetDesc();
            const uint32_t count = desc.MipLevels * desc.DepthOrArraySize;
            flight.AssetStates.emplace_back(count, render::TextureState::ShaderRead);
            flight.AssetValid.emplace_back(count, 1);
            texture = import(native.Get(), flight.AssetStates.back(), flight.AssetValid.back(), false);
        }
        if (texture.IsValid()) bindings.emplace(id, RgTextureParameterBinding{texture, record.Descriptor.View});
    }
    for (const auto& image : images) {
        auto record = flight.Textures.find(image.Image);
        const auto descriptor = graph.GetTextureDescriptor(image.Texture);
        if (record == flight.Textures.end() || !record->second.Graph || !descriptor || !bindings.emplace(image.Image, RgTextureParameterBinding{image.Texture, image.View}).second)
            graph.AddDiagnostic("ImGuiGraphImage", "Graph image binding is missing, duplicated, stale or belongs to another graph");
    }
    for (const auto& viewport : flight.Viewports) {
        const auto target = FindGraphOutput(targets, viewport.Output);
        if (!target) continue;
        const auto desc = graph.GetTextureDescriptor(target->Texture);
        if (!desc) continue;
        auto& canvas = target->Texture;
        if (!viewport.Vertices.empty() && !viewport.Indices.empty()) {
            const auto vertices = graph.UploadBuffer("ImGui.Vertices", std::as_bytes(std::span{viewport.Vertices}), render::BufferUse::Vertex);
            const auto indices = graph.UploadBuffer("ImGui.Indices", std::as_bytes(std::span{viewport.Indices}), render::BufferUse::Index);
            struct Draw {
                RgParameterSetHandle Set;
                Rect Clip;
                uint32_t Count, Index;
                int32_t Vertex;
            };
            struct Data {
                RgGraphicsProgramHandle Program;
                RgBufferValue Vertices, Indices;
                uint64_t VertexBytes;
                uint32_t Width, Height;
                render::RenderBackend Backend;
                vector<Draw> Draws;
            };
            canvas = graph.NextVersion(canvas);
            graph.AddRasterPass<Data>("ImGui.Draw", [&](Data& data, RenderGraphRasterBuilder& builder) {
                data.Program = builder.UseGraphicsProgram(*self.DrawProgram, UiState(true), UiLayout()); data.Vertices = builder.ReadBuffer(vertices, RgBufferAccess::Vertex); data.Indices = builder.ReadBuffer(indices, RgBufferAccess::Index);
                data.VertexBytes = viewport.Vertices.size() * sizeof(ImDrawVert); data.Width = desc->Width; data.Height = desc->Height; data.Backend = device.GetBackend();
                builder.SetColorAttachment(0, canvas, {.Load = render::LoadAction::Load});
                for (const auto& draw : viewport.Commands) {
                    auto found = bindings.find(draw.Texture);
                    if (found == bindings.end()) { graph.AddDiagnostic("ImGuiImage", "Image has no texture binding in this frame"); continue; }
                    const auto textureDesc = graph.GetTextureDescriptor(found->second.Texture);
                    if (!textureDesc || textureDesc->SampleCount != 1 || !textureDesc->Usage.HasFlag(render::TextureUse::Resource) ||
                        textureDesc->Dim != render::TextureDimension::Dim2D) {
                        graph.AddDiagnostic("ImGuiImage", "Image requires a sampleable single-sample 2D texture without attachment feedback; resolve MSAA explicitly"); continue;
                    }
                    if (draw.IndexOffset > viewport.Indices.size() || draw.Count > viewport.Indices.size() - draw.IndexOffset || draw.VertexOffset < 0) {
                        graph.AddDiagnostic("ImGuiDraw", "Draw offsets exceed the owned index snapshot"); continue;
                    }
                    bool indicesValid = true;
                    for (size_t i = draw.IndexOffset; i < size_t(draw.IndexOffset) + draw.Count; ++i)
                        indicesValid &= uint64_t(draw.VertexOffset) + viewport.Indices[i] < viewport.Vertices.size();
                    if (!indicesValid) { graph.AddDiagnostic("ImGuiDraw", "Draw indices exceed the owned vertex snapshot"); continue; }
                    const auto& r = draw.Clip;
                    if (!std::isfinite(r.x) || !std::isfinite(r.y) || !std::isfinite(r.z) || !std::isfinite(r.w)) { graph.AddDiagnostic("ImGuiDraw", "Clip rectangle is not finite"); continue; }
                    const float left = std::clamp((r.x - viewport.Position.x) * viewport.Scale.x, 0.0f, float(desc->Width));
                    const float top = std::clamp((r.y - viewport.Position.y) * viewport.Scale.y, 0.0f, float(desc->Height));
                    const float right = std::clamp((r.z - viewport.Position.x) * viewport.Scale.x, 0.0f, float(desc->Width));
                    const float bottom = std::clamp((r.w - viewport.Position.y) * viewport.Scale.y, 0.0f, float(desc->Height));
                    if (right <= left || bottom <= top) continue;
                    const auto& texture = flight.Textures.at(draw.Texture);
                    const auto encoding = outputEncodings.contains(draw.Texture) ? outputEncodings.at(draw.Texture) : texture.Descriptor.Encoding;
                    UiConstants values{{2 / viewport.Size.x, -2 / viewport.Size.y, -1 - viewport.Position.x * 2 / viewport.Size.x, 1 + viewport.Position.y * 2 / viewport.Size.y},
                                       {encoding == ImGuiColorEncoding::Srgb ? 1.0f : 0.0f, 0, 0, 0}};
                    const auto sampler = draw.Sampler == 0 && texture.Descriptor.Sampler ? *texture.Descriptor.Sampler : Sampler(draw.Sampler);
                    const RgParameterBinding parameters[]{
                        {"Ui", 0, RgCBufferParameterBinding{std::as_bytes(std::span{&values, 1})}},
                        {"Image", 0, found->second}, {"ImageSampler", 0, RgSamplerParameterBinding{sampler}}};
                    const auto set = builder.CreateParameterSet(*self.DrawProgram, 0, parameters);
                    const int32_t x = int32_t(std::floor(left)), y = int32_t(std::floor(top));
                    data.Draws.push_back({set, {x, y, uint32_t(std::ceil(right)) - uint32_t(x), uint32_t(std::ceil(bottom)) - uint32_t(y)}, draw.Count, draw.IndexOffset, draw.VertexOffset});
                } }, +[](const Data& data, RenderGraphRasterContext& ctx) {
                auto& encoder = ctx.Encoder();
                ctx.BindGraphicsProgram(data.Program);
                const render::VertexBufferBinding vertex{0, {ctx.GetBuffer(data.Vertices), 0, data.VertexBytes}};
                encoder.BindVertexBuffers(std::span{&vertex, 1}); encoder.BindIndexBuffer({ctx.GetBuffer(data.Indices), 0, sizeof(ImDrawIdx)});
                encoder.SetViewport(MakeViewport(data.Backend, 0, 0, float(data.Width), float(data.Height)));
                for (const auto& draw : data.Draws) { ctx.BindParameterSet(draw.Set); encoder.SetScissor(draw.Clip); encoder.DrawIndexed(draw.Count, 1, draw.Index, draw.Vertex, 0); } });
        }
    }
    return !graph.HasFailed();
}
void ImGuiGraph::CompleteGraph(const RenderGraph& graph, RenderPipelineContext& context, ImGuiGraphFrame frame, bool success) {
    RADRAY_ASSERT(frame._index == context.FlightIndex());
    (void)context;
    auto& self = frame._resources;
    auto& flight = frame._flight;
    (void)graph;
    for (const auto& ticket : flight.UploadTickets) success &= ticket.Status() == FrameOperationStatus::Recorded;
    flight.GraphSuccess = success;
    if (!success) {
        self.Error = true;
        return;
    }
    for (const auto& request : flight.Requests)
        if (request.Status == ImTextureStatus_WantDestroy) self.GpuTextures.erase(request.Id);
}

void ImGuiGraphComponent::BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) {
    const auto frame = _system.GetGraphFrame(context.FlightIndex());
    vector<ImGuiSceneOutput> scenes{OutputImages.begin(), OutputImages.end()};
    // Registered outputs without an explicit preview image sample a snapshot of the incoming
    // target version; the same display storage advances while the UI draws over it.
    for (const auto& target : outputs) {
        if (std::any_of(scenes.begin(), scenes.end(), [&](const auto& scene) { return scene.Output == target.Output; })) continue;
        bool registered = false;
        for (const auto& [id, texture] : frame._flight.Textures) {
            (void)id;
            registered |= texture.Output == target.Output;
        }
        if (!registered) continue;
        const auto desc = graph.GetTextureDescriptor(target.Texture);
        if (!desc) continue;
        const bool unorm = desc->Format == render::TextureFormat::RGBA8_UNORM || desc->Format == render::TextureFormat::BGRA8_UNORM;
        auto snapshotDesc = *desc;
        snapshotDesc.Usage = render::TextureUse::Resource | render::TextureUse::CopyDestination;
        const auto image = graph.CreateTexture(snapshotDesc, "ImGui.OutputSnapshot");
        graph.AddCopyTexturePass("ImGui.OutputSnapshot", target.Texture, image);
        scenes.push_back({target.Output, image, unorm ? ImGuiColorEncoding::Srgb : ImGuiColorEncoding::Linear});
    }
    ImGuiGraph::BuildGraph(graph, context, frame, outputs, scenes, Images);
}
void ImGuiGraphComponent::GraphRecorded(RenderPipelineContext& context, const RenderGraph& graph, RenderGraphExecutionResult result) {
    ImGuiGraph::CompleteGraph(graph, context, _system.GetGraphFrame(context.FlightIndex()), result.Success);
}

}  // namespace radray
