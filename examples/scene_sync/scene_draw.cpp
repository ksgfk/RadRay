#include "scene_draw.h"

#include <cstring>
#include <radray/profiler.h>

namespace radray::example {

bool SceneDraw::Initialize(RenderSystem& renderer, render::Device* device, render::RenderPass* pass, render::TextureFormat format, uint32_t flights) {
    auto program = renderer.GetOrCreateShaderProgram({.SourceName = "scene_sync.hlsl"});
    if (!program) return false;
    auto* layout = program->GetPipelineLayout();
    _layout = layout;
    _objects = layout->FindBinding("Objects");
    _view = layout->FindBinding("ViewData");
    _draw = layout->FindBinding("DrawData");
    const auto objectInfo = program->GetArtifact().FindBindingInfo("Objects");
    const auto viewInfo = program->GetArtifact().FindBindingInfo("ViewData");
    if (!_objects.IsValid() || !_view.IsValid() || !_draw.IsValid() || !objectInfo || !viewInfo || objectInfo->Group != viewInfo->Group) return false;
    _group = objectInfo->Group;
    const render::VertexBufferLayout buffer{.Binding = 0, .ArrayStride = 12, .StepMode = render::VertexStepMode::Vertex};
    const render::VertexAttribute attribute{.BufferBinding = 0, .Semantic = "POSITION", .Format = render::VertexFormat::FLOAT32X3, .Location = 0};
    const render::VertexInputState input{std::span{&buffer, 1}, std::span{&attribute, 1}};
    const auto color = render::ColorTargetState::Default(format);
    for (uint32_t reverse = 0; reverse < 2; ++reverse) {
        auto primitive = render::PrimitiveState::Default();
        primitive.FaceClockwise = reverse ? render::FrontFace::CCW : render::FrontFace::CW;
        auto pipeline = device->CreateGraphicsPipelineState({.PipelineLayout = layout,
                                                             .VS = program->GetStage(shader::ShaderStage::Vertex),
                                                             .PS = program->GetStage(shader::ShaderStage::Pixel),
                                                             .VertexInput = input,
                                                             .Primitive = primitive,
                                                             .DepthStencil = std::nullopt,
                                                             .MultiSample = render::MultiSampleState::Default(),
                                                             .ColorTargets = std::span{&color, 1},
                                                             .CompatibleRenderPass = pass});
        if (!pipeline) return false;
        _pipelines[reverse] = pipeline.Release();
    }
    _flights.resize(flights);
    return true;
}

bool SceneDraw::Draw(SceneManager& scenes, AppFrameContext& frame, render::GraphicsCommandEncoder* encoder,
                     const SceneViewRequest& request, const SceneGpuView& objects, uint32_t viewIndex, uint32_t width, uint32_t height) {
    RADRAY_PROFILE_SCOPE_N("SceneDraw::Draw");
    const auto resolved = ResolveSceneView(request, width, height, frame.GetDevice()->GetBackend());
    auto scene = scenes.GetSceneRT(request.Scene);
    if (!resolved || !scene) return false;
    (void)viewIndex;
    auto& flight = _flights[frame.FlightIndex()];
    if (flight.Serial != frame.FrameSerial()) {
        flight.Serial = frame.FrameSerial();
        flight.Used = 0;
    }
    if (flight.Used == flight.Views.size()) {
        auto buffer = frame.GetDevice()->CreateBuffer({.Size = std::max(64u, frame.GetDevice()->GetDetail().CBufferAlignment), .Memory = render::MemoryType::Upload, .Usage = render::BufferUse::CBuffer | render::BufferUse::MapWrite, .Hints = render::ResourceHint::PersistentMap});
        auto parameters = frame.GetDevice()->CreateShaderParameterSet({_layout.Get(), _group});
        if (!buffer || !parameters) return false;
        ViewResources next{make_unique<MappedUploadPage>(buffer.Release()), parameters.Release()};
        if (!next.Parameters->Set(_view, 0, render::ShaderBufferBinding{next.Constants->GetBuffer(), {0, 64}, 0})) return false;
        flight.Views.push_back(std::move(next));
    }
    auto& view = flight.Views[flight.Used++];
    auto constants = view.Constants->ReserveAt(0, 64, frame.GetHostWrites());
    if (!constants.IsValid()) return false;
    std::memcpy(constants.Data(), resolved->ViewProjection.data(), 64);
    constants.Commit(64);
    if (!view.Parameters->Set(_objects, 0, objects.Objects) || !view.Parameters->FlushWrites()) return false;
    encoder->SetViewport(resolved->Viewport);
    encoder->SetScissor(resolved->Scissor);
    const auto columns = scene->GetStaticMeshColumns();
    for (size_t row = 0; row < columns.Size(); ++row) {
        const auto object = columns.Get(row);
        const auto mesh = object.Mesh.GetRenderMesh();
        if (!mesh) continue;
        encoder->BindGraphicsPipelineState(_pipelines[object.ReverseCulling ? 1 : 0].get());
        encoder->BindShaderParameterSet(_group, view.Parameters.get());
        const uint32_t slot = columns.Ids[row].Index;
        if (!encoder->SetPushConstants(_draw, std::as_bytes(std::span{&slot, 1}))) return false;
        for (const auto& section : object.Mesh.GetSections()) {
            if (section.IndexCount == 0 || section.PrimitiveIndex >= mesh->Draws.size()) continue;
            const auto& draw = mesh->Draws[section.PrimitiveIndex];
            if (draw.VertexBuffers.empty() || draw.Ibv.Target == nullptr) continue;
            encoder->BindVertexBuffers(draw.VertexBuffers);
            encoder->BindIndexBuffer(draw.Ibv);
            encoder->DrawIndexed(section.IndexCount, 1, section.FirstIndex, section.VertexOffset, 0);
        }
    }
    return true;
}

unique_ptr<StaticMesh> CreateCube(GpuSystem& gpu) {
    const array<float, 24> vertices{-0.5f, -0.5f, -0.5f, 0.5f, -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f,
                                    -0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0.5f, -0.5f, 0.5f, 0.5f};
    const array<uint32_t, 36> indices{0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
    MeshResource cpu;
    cpu.Bins.emplace_back(std::as_bytes(std::span{vertices}));
    cpu.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 8;
    primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 36, 0, 4};
    cpu.Primitives.push_back(std::move(primitive));
    auto* device = gpu.GetDevice();
    auto commandsStorage = device->CreateCommandAllocator(gpu.GetMainQueue());
    if (!commandsStorage) return nullptr;
    auto commands = device->CreateCommandBuffer(commandsStorage.Get());
    auto fence = device->CreateFence();
    if (!commands || !fence) return nullptr;
    ResourceUploader uploader{device, 1};
    HostWriteBatch writes;
    uploader.BeginFlight(0, writes);
    commands->Begin();
    auto geometry = uploader.UploadMeshResource(commands.Get(), cpu);
    commands->End();
    uploader.EndFlight(0);
    if (!geometry) return nullptr;
    writes.Flush(*device);
    auto* commandPtr = commands.Get();
    auto* fencePtr = fence.Get();
    uint64_t signal = 1;
    gpu.GetMainQueue()->Submit({.CmdBuffers = std::span{&commandPtr, 1}, .SignalFences = std::span{&fencePtr, 1}, .SignalValues = std::span{&signal, 1}});
    fence->Wait(signal);
    uploader.CollectFlight(0);
    return make_unique<StaticMesh>(std::move(cpu), vector<StaticMeshSection>{}, Eigen::Vector3f{-0.5f, -0.5f, -0.5f}, Eigen::Vector3f{0.5f, 0.5f, 0.5f}, std::move(*geometry));
}

}  // namespace radray::example
