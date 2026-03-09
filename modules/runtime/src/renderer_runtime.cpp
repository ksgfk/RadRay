#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>

#include <fmt/format.h>

#include <radray/logger.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/render/shader_compiler/spvc.h>

#include <radray/runtime/renderer_runtime.h>

namespace radray::runtime {
namespace {

constexpr std::string_view kForwardShaderSource = R"(
struct DrawConstants {
    float4x4 LocalToWorld;
    float4 Tint;
};

struct FrameConstants {
    float4x4 ViewProj;
};

[[vk::push_constant]] ConstantBuffer<DrawConstants> gDraw : register(b0, space0);
[[vk::binding(1, 0)]] ConstantBuffer<FrameConstants> gFrame : register(b1, space0);
[[vk::binding(0, 1)]] Texture2D<float4> gAlbedo : register(t0, space1);
[[vk::binding(1, 1)]] SamplerState gAlbedoSampler : register(s1, space1);

struct VSInput {
    float3 Position : POSITION;
    float2 UV : TEXCOORD0;
};

struct VSOutput {
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    const float4 world = mul(gDraw.LocalToWorld, float4(input.Position, 1.0));
    output.Position = mul(gFrame.ViewProj, world);
    output.UV = input.UV;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
    return gAlbedo.Sample(gAlbedoSampler, input.UV) * gDraw.Tint;
}
)";

struct DrawConstantsCpu {
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
    Eigen::Vector4f Tint{Eigen::Vector4f::Ones()};
};

struct FrameConstantsCpu {
    Eigen::Matrix4f ViewProj{Eigen::Matrix4f::Identity()};
};

static_assert(sizeof(DrawConstantsCpu) == 80);
static_assert(sizeof(FrameConstantsCpu) == 64);

struct PipelineKey {
    render::TextureFormat TargetFormat{render::TextureFormat::UNKNOWN};
    uint64_t ArrayStride{0};
    uint64_t PositionOffset{0};
    render::VertexFormat PositionFormat{render::VertexFormat::UNKNOWN};
    uint64_t UvOffset{0};
    render::VertexFormat UvFormat{render::VertexFormat::UNKNOWN};

    friend auto operator<=>(const PipelineKey& lhs, const PipelineKey& rhs) noexcept = default;
};

struct FrameSlot {
    FrameInFlight PublicState{};
    unique_ptr<render::CommandBuffer> CommandBuffer{};
    unique_ptr<render::Fence> Fence{};
    render::CBufferArena CBufferArena;
    vector<unique_ptr<render::BufferView>> TemporaryBufferViews{};
    vector<unique_ptr<render::DescriptorSet>> TemporaryDescriptorSets{};
    unique_ptr<render::Buffer> CaptureReadbackBuffer{};
    uint64_t CaptureRowPitch{0};
    uint64_t CaptureSize{0};
    uint32_t CaptureWidth{0};
    uint32_t CaptureHeight{0};
    render::TextureFormat CaptureFormat{render::TextureFormat::UNKNOWN};

    explicit FrameSlot(render::Device* device) noexcept
        : CBufferArena(device, render::CBufferArena::Descriptor{
                                   .BasicSize = 256 * 256,
                                   .Alignment = std::max<uint64_t>(256, device->GetDetail().CBufferAlignment),
                                   .MaxResetSize = 1024 * 1024,
                                   .NamePrefix = "runtime_frame_cb"}) {}
};

struct GraphicsProgram {
    shared_ptr<render::Dxc> DxcCompiler{};
    vector<byte> VertexBlob{};
    vector<byte> PixelBlob{};
    unique_ptr<render::Shader> VertexShader{};
    unique_ptr<render::Shader> PixelShader{};
    unique_ptr<render::RootSignature> RootSignature{};
    std::map<PipelineKey, unique_ptr<render::GraphicsPipelineState>> PipelineStates{};
    std::unordered_map<uint32_t, unique_ptr<render::DescriptorSet>> MaterialDescriptorSets{};
    std::optional<render::BindingParameterId> DrawConstantsId{};
    std::optional<render::BindingParameterId> FrameConstantsId{};
    std::optional<render::BindingParameterId> AlbedoId{};
    std::optional<render::BindingParameterId> AlbedoSamplerId{};
};

bool StoreReason(Nullable<string*> reason, std::string_view message) noexcept {
    if (reason.HasValue()) {
        *reason.Get() = string{message};
    }
    return false;
}

bool AppendHostWriteBarrier(
    render::Device* device,
    vector<render::ResourceBarrierDescriptor>& barriers,
    render::Buffer* buffer,
    render::BufferStates after) {
    if (device == nullptr || buffer == nullptr) {
        return true;
    }
    if (device->GetBackend() != render::RenderBackend::Vulkan) {
        return true;
    }
    barriers.push_back(render::BarrierBufferDescriptor{
        .Target = buffer,
        .Before = render::BufferState::HostWrite,
        .After = after,
    });
    return true;
}

void AppendReadbackPreCopyBarrier(
    render::Device* device,
    vector<render::ResourceBarrierDescriptor>& barriers,
    render::Buffer* buffer) {
    if (device == nullptr || buffer == nullptr || device->GetBackend() != render::RenderBackend::Vulkan) {
        return;
    }
    barriers.push_back(render::BarrierBufferDescriptor{
        .Target = buffer,
        .Before = render::BufferState::Common,
        .After = render::BufferState::CopyDestination,
    });
}

void AppendReadbackPostCopyBarrier(
    render::Device* device,
    vector<render::ResourceBarrierDescriptor>& barriers,
    render::Buffer* buffer) {
    if (device == nullptr || buffer == nullptr || device->GetBackend() != render::RenderBackend::Vulkan) {
        return;
    }
    barriers.push_back(render::BarrierBufferDescriptor{
        .Target = buffer,
        .Before = render::BufferState::CopyDestination,
        .After = render::BufferState::HostRead,
    });
}

}  // namespace

class UploadSystem::Impl {
public:
    render::Device* Device{nullptr};
    std::unordered_map<uint32_t, vector<unique_ptr<render::Buffer>>> RetainedUploadBuffers{};
};

namespace {

bool CreateShaderFromDxcOutput(
    render::Device* device,
    render::RenderBackend backend,
    shared_ptr<render::Dxc> dxc,
    std::string_view source,
    std::string_view entryPoint,
    render::ShaderStage stage,
    vector<byte>& blobStorage,
    unique_ptr<render::Shader>& shaderOut,
    Nullable<string*> reason) {
    if (device == nullptr || dxc == nullptr) {
        return StoreReason(reason, "device or dxc compiler is not initialized");
    }

    render::DxcCompileParams params{};
    params.Code = source;
    params.EntryPoint = entryPoint;
    params.Stage = stage;
    params.SM = render::HlslShaderModel::SM60;
    params.IsOptimize = false;
    params.IsSpirv = backend == render::RenderBackend::Vulkan;
    params.EnableUnbounded = false;
    auto outputOpt = dxc->Compile(params);
    if (!outputOpt.has_value()) {
        return StoreReason(reason, fmt::format("DXC compile failed for '{}'", entryPoint));
    }

    render::ShaderReflectionDesc reflection{};
    render::ShaderBlobCategory category = render::ShaderBlobCategory::DXIL;
    if (backend == render::RenderBackend::Vulkan) {
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
        auto reflectionOpt = render::ReflectSpirv(render::SpirvBytecodeView{
            .Data = outputOpt->Data,
            .EntryPointName = entryPoint,
            .Stage = stage,
        });
        if (!reflectionOpt.has_value()) {
            return StoreReason(reason, "SPIR-V reflection extraction failed");
        }
        reflection = std::move(reflectionOpt.value());
        category = render::ShaderBlobCategory::SPIRV;
#else
        return StoreReason(reason, "SPIR-V Cross is not enabled for Vulkan runtime shaders");
#endif
    } else {
        auto reflectionOpt = dxc->GetShaderDescFromOutput(outputOpt->Refl);
        if (!reflectionOpt.has_value()) {
            return StoreReason(reason, "DXIL reflection extraction failed");
        }
        reflection = std::move(reflectionOpt.value());
        category = render::ShaderBlobCategory::DXIL;
    }

    blobStorage = std::move(outputOpt->Data);
    render::ShaderDescriptor shaderDesc{};
    shaderDesc.Source = std::span<const byte>{blobStorage.data(), blobStorage.size()};
    shaderDesc.Category = category;
    shaderDesc.Stages = stage;
    shaderDesc.Reflection = std::move(reflection);
    auto shaderOpt = device->CreateShader(shaderDesc);
    if (!shaderOpt.HasValue()) {
        return StoreReason(reason, fmt::format("CreateShader failed for '{}'", entryPoint));
    }
    shaderOut = shaderOpt.Release();
    return true;
}

std::optional<PipelineKey> BuildPipelineKey(const MeshSubmesh& submesh, render::TextureFormat format) {
    std::optional<render::VertexElement> position{};
    std::optional<render::VertexElement> uv{};
    for (const auto& element : submesh.VertexElements) {
        if (element.Semantic == VertexSemantics::POSITION && element.SemanticIndex == 0) {
            position = element;
        } else if (element.Semantic == VertexSemantics::TEXCOORD && element.SemanticIndex == 0) {
            uv = element;
        }
    }
    if (!position.has_value() || !uv.has_value()) {
        return std::nullopt;
    }
    return PipelineKey{
        .TargetFormat = format,
        .ArrayStride = submesh.VertexLayout.ArrayStride,
        .PositionOffset = position->Offset,
        .PositionFormat = position->Format,
        .UvOffset = uv->Offset,
        .UvFormat = uv->Format,
    };
}

class RendererRuntimeImpl {
public:
    shared_ptr<render::Device> Device{};
    render::CommandQueue* Queue{nullptr};
    unique_ptr<render::SwapChain> Swapchain{};
    unique_ptr<RenderAssetRegistry> AssetRegistry{};
    UploadSystem Uploader{};
    GraphicsProgram Program{};
    vector<FrameSlot> Frames{};
    vector<render::TextureState> BackBufferStates{};
    vector<unique_ptr<render::TextureView>> BackBufferRtvs{};
    vector<render::Texture*> BackBufferTargets{};
    uint32_t Width{0};
    uint32_t Height{0};
    uint32_t BackBufferCount{0};
    uint32_t FlightFrameCount{0};
    uint64_t FrameCounter{0};
    render::TextureFormat RequestedFormat{render::TextureFormat::UNKNOWN};
    render::TextureFormat ActiveFormat{render::TextureFormat::UNKNOWN};
    render::PresentMode PresentMode{render::PresentMode::FIFO};
    const void* NativeHandler{nullptr};
    bool CaptureEnabled{false};
    std::optional<uint32_t> LastCaptureSlot{};

    bool InitializeProgram(Nullable<string*> reason) noexcept;

    bool EnsureBackBufferRtv(uint32_t backBufferIndex, render::Texture* backBuffer, Nullable<string*> reason) noexcept;

    bool CreateSwapchain(Nullable<string*> reason) noexcept;

    render::GraphicsPipelineState* GetOrCreatePipeline(const MeshSubmesh& submesh, Nullable<string*> reason) noexcept;

    render::DescriptorSet* GetOrCreateMaterialSet(MaterialHandle handle, Nullable<string*> reason) noexcept;

    vector<PreparedView> PrepareViews(const FrameSnapshot& snapshot) const;

    const CameraRenderData* FindCamera(const FrameSnapshot& snapshot, uint32_t cameraId) const noexcept;
};

}  // namespace

UploadSystem::UploadSystem() noexcept = default;

UploadSystem::~UploadSystem() noexcept {
    this->Destroy();
}

bool UploadSystem::Initialize(render::Device* device) noexcept {
    _impl = make_unique<Impl>();
    _impl->Device = device;
    return device != nullptr;
}

void UploadSystem::Destroy() noexcept {
    _impl.reset();
}

bool UploadSystem::ProcessPendingUploads(
    render::CommandBuffer* cmd,
    RenderAssetRegistry& assets,
    FrameInFlight& frame,
    Nullable<string*> reason) noexcept {
    if (_impl == nullptr || _impl->Device == nullptr) {
        return StoreReason(reason, "UploadSystem is not initialized");
    }
    if (cmd == nullptr) {
        return StoreReason(reason, "command buffer is null");
    }

    const auto pendingUploads = assets.GetPendingUploads();
    vector<PendingUploadHandle> toProcess(pendingUploads.begin(), pendingUploads.end());
    auto& retainedBuffers = _impl->RetainedUploadBuffers[frame.FrameIndex];

    for (const PendingUploadHandle& pending : toProcess) {
        if (pending.UploadKind == PendingUploadHandle::Kind::Mesh) {
            auto meshOpt = assets.ResolveMesh(MeshHandle{pending.Value});
            if (!meshOpt.HasValue()) {
                return StoreReason(reason, "pending mesh upload references an invalid handle");
            }
            auto* mesh = meshOpt.Get();
            for (size_t index = 0; index < mesh->Buffers.size(); ++index) {
                const auto& uploadData = mesh->PendingUploadData[index];
                if (uploadData.empty()) {
                    continue;
                }
                render::BufferDescriptor uploadDesc{};
                uploadDesc.Size = uploadData.size();
                uploadDesc.Memory = render::MemoryType::Upload;
                uploadDesc.Usage = render::BufferUse::MapWrite | render::BufferUse::CopySource;
                auto uploadOpt = _impl->Device->CreateBuffer(uploadDesc);
                if (!uploadOpt.HasValue()) {
                    return StoreReason(reason, "CreateBuffer failed for mesh upload staging");
                }
                auto upload = uploadOpt.Release();
                upload->SetDebugName(fmt::format("runtime_mesh_upload_{}_{}", pending.Value, index));
                void* mapped = upload->Map(0, uploadData.size());
                if (mapped == nullptr) {
                    return StoreReason(reason, "Map failed for mesh upload staging buffer");
                }
                std::memcpy(mapped, uploadData.data(), uploadData.size());
                upload->Unmap(0, uploadData.size());

                vector<render::ResourceBarrierDescriptor> preCopy{};
                AppendHostWriteBarrier(_impl->Device, preCopy, upload.get(), render::BufferState::CopySource);
                preCopy.push_back(render::BarrierBufferDescriptor{
                    .Target = mesh->Buffers[index].get(),
                    .Before = render::BufferState::Common,
                    .After = render::BufferState::CopyDestination,
                });
                cmd->ResourceBarrier(preCopy);
                cmd->CopyBufferToBuffer(mesh->Buffers[index].get(), 0, upload.get(), 0, uploadData.size());

                if (mesh->PendingFinalStates[index] != render::BufferState::CopyDestination) {
                    render::ResourceBarrierDescriptor postCopy = render::BarrierBufferDescriptor{
                        .Target = mesh->Buffers[index].get(),
                        .Before = render::BufferState::CopyDestination,
                        .After = mesh->PendingFinalStates[index],
                    };
                    cmd->ResourceBarrier(std::span{&postCopy, 1});
                }
                retainedBuffers.push_back(std::move(upload));
                mesh->PendingUploadData[index].clear();
            }
            mesh->IsUploaded = true;
            assets.RemovePendingUpload(pending);
            continue;
        }

        auto textureOpt = assets.ResolveTexture(TextureHandle{pending.Value});
        if (!textureOpt.HasValue()) {
            return StoreReason(reason, "pending texture upload references an invalid handle");
        }
        auto* texture = textureOpt.Get();
        if (texture->PendingUploadData.empty()) {
            texture->IsUploaded = true;
            assets.RemovePendingUpload(pending);
            continue;
        }

        const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(texture->Desc.Format);
        if (bytesPerPixel == 0) {
            return StoreReason(reason, "texture upload format is unsupported");
        }
        const uint64_t expectedBytes = static_cast<uint64_t>(texture->Desc.Width) * texture->Desc.Height * bytesPerPixel;
        if (texture->PendingUploadData.size() != expectedBytes) {
            return StoreReason(reason, "texture upload size mismatch");
        }
        const uint64_t rowPitchAlignment = std::max<uint64_t>(1, _impl->Device->GetDetail().TextureDataPitchAlignment);
        const uint64_t bytesPerRow = static_cast<uint64_t>(texture->Desc.Width) * bytesPerPixel;
        const uint64_t alignedBytesPerRow = Align(bytesPerRow, rowPitchAlignment);
        vector<byte> staging(alignedBytesPerRow * texture->Desc.Height, byte{0});
        for (uint32_t row = 0; row < texture->Desc.Height; ++row) {
            const auto* src = texture->PendingUploadData.data() + row * bytesPerRow;
            auto* dst = staging.data() + row * alignedBytesPerRow;
            std::memcpy(dst, src, bytesPerRow);
        }

        render::BufferDescriptor uploadDesc{};
        uploadDesc.Size = staging.size();
        uploadDesc.Memory = render::MemoryType::Upload;
        uploadDesc.Usage = render::BufferUse::MapWrite | render::BufferUse::CopySource;
        auto uploadOpt = _impl->Device->CreateBuffer(uploadDesc);
        if (!uploadOpt.HasValue()) {
            return StoreReason(reason, "CreateBuffer failed for texture upload staging");
        }
        auto upload = uploadOpt.Release();
        upload->SetDebugName(fmt::format("runtime_texture_upload_{}", pending.Value));
        void* mapped = upload->Map(0, staging.size());
        if (mapped == nullptr) {
            return StoreReason(reason, "Map failed for texture upload staging buffer");
        }
        std::memcpy(mapped, staging.data(), staging.size());
        upload->Unmap(0, staging.size());

        vector<render::ResourceBarrierDescriptor> preCopy{};
        AppendHostWriteBarrier(_impl->Device, preCopy, upload.get(), render::BufferState::CopySource);
        preCopy.push_back(render::BarrierTextureDescriptor{
            .Target = texture->TextureObject.get(),
            .Before = render::TextureState::Undefined,
            .After = render::TextureState::CopyDestination,
        });
        cmd->ResourceBarrier(preCopy);
        cmd->CopyBufferToTexture(texture->TextureObject.get(), render::SubresourceRange{0, 1, 0, 1}, upload.get(), 0);

        render::ResourceBarrierDescriptor postCopy = render::BarrierTextureDescriptor{
            .Target = texture->TextureObject.get(),
            .Before = render::TextureState::CopyDestination,
            .After = render::TextureState::ShaderRead,
        };
        cmd->ResourceBarrier(std::span{&postCopy, 1});
        retainedBuffers.push_back(std::move(upload));
        texture->PendingUploadData.clear();
        texture->IsUploaded = true;
        assets.RemovePendingUpload(pending);
    }
    return true;
}

void UploadSystem::ReleaseFrameResources(FrameInFlight& frame) noexcept {
    if (_impl == nullptr) {
        return;
    }
    _impl->RetainedUploadBuffers.erase(frame.FrameIndex);
}

namespace {

bool RendererRuntimeImpl::InitializeProgram(Nullable<string*> reason) noexcept {
#if !defined(RADRAY_ENABLE_DXC)
    return StoreReason(reason, "runtime fixed pipeline requires DXC");
#else
    if (Program.RootSignature != nullptr) {
        return true;
    }
    auto dxcOpt = render::CreateDxc();
    if (!dxcOpt.HasValue()) {
        return StoreReason(reason, "CreateDxc failed");
    }
    Program.DxcCompiler = dxcOpt.Release();
    if (!CreateShaderFromDxcOutput(
            Device.get(),
            Device->GetBackend(),
            Program.DxcCompiler,
            kForwardShaderSource,
            "VSMain",
            render::ShaderStage::Vertex,
            Program.VertexBlob,
            Program.VertexShader,
            reason)) {
        return false;
    }
    if (!CreateShaderFromDxcOutput(
            Device.get(),
            Device->GetBackend(),
            Program.DxcCompiler,
            kForwardShaderSource,
            "PSMain",
            render::ShaderStage::Pixel,
            Program.PixelBlob,
            Program.PixelShader,
            reason)) {
        return false;
    }
    render::Shader* shaders[] = {
        Program.VertexShader.get(),
        Program.PixelShader.get(),
    };
    render::RootSignatureDescriptor rootSignatureDesc{};
    rootSignatureDesc.Shaders = shaders;
    auto rootSigOpt = Device->CreateRootSignature(rootSignatureDesc);
    if (!rootSigOpt.HasValue()) {
        return StoreReason(reason, "CreateRootSignature failed for fixed forward pipeline");
    }
    Program.RootSignature = rootSigOpt.Release();
    Program.RootSignature->SetDebugName("runtime_forward_root_signature");
    Program.DrawConstantsId = Program.RootSignature->FindParameterId("gDraw");
    Program.FrameConstantsId = Program.RootSignature->FindParameterId("gFrame");
    Program.AlbedoId = Program.RootSignature->FindParameterId("gAlbedo");
    Program.AlbedoSamplerId = Program.RootSignature->FindParameterId("gAlbedoSampler");
    if (!Program.DrawConstantsId.has_value() ||
        !Program.FrameConstantsId.has_value() ||
        !Program.AlbedoId.has_value() ||
        !Program.AlbedoSamplerId.has_value()) {
        return StoreReason(reason, "fixed forward pipeline parameter lookup failed");
    }
    return true;
#endif
}

bool RendererRuntimeImpl::EnsureBackBufferRtv(uint32_t backBufferIndex, render::Texture* backBuffer, Nullable<string*> reason) noexcept {
    if (backBufferIndex >= BackBufferRtvs.size()) {
        return StoreReason(reason, "back buffer index out of range");
    }
    if (BackBufferRtvs[backBufferIndex] != nullptr &&
        BackBufferTargets[backBufferIndex] == backBuffer) {
        return true;
    }

    render::TextureViewDescriptor rtvDesc{};
    rtvDesc.Target = backBuffer;
    rtvDesc.Dim = render::TextureDimension::Dim2D;
    rtvDesc.Format = backBuffer->GetDesc().Format;
    rtvDesc.Range = render::SubresourceRange{0, 1, 0, 1};
    rtvDesc.Usage = render::TextureViewUsage::RenderTarget;
    auto rtvOpt = Device->CreateTextureView(rtvDesc);
    if (!rtvOpt.HasValue()) {
        return StoreReason(reason, "CreateTextureView for swapchain back buffer failed");
    }
    BackBufferRtvs[backBufferIndex] = rtvOpt.Release();
    BackBufferRtvs[backBufferIndex]->SetDebugName(fmt::format("runtime_backbuffer_rtv_{}", backBufferIndex));
    BackBufferTargets[backBufferIndex] = backBuffer;
    return true;
}

bool RendererRuntimeImpl::CreateSwapchain(Nullable<string*> reason) noexcept {
    if (Device == nullptr || Queue == nullptr || NativeHandler == nullptr || Width == 0 || Height == 0) {
        return StoreReason(reason, "invalid swapchain creation arguments");
    }

    vector<render::TextureFormat> formats{};
    formats.push_back(RequestedFormat);
    if (RequestedFormat == render::TextureFormat::BGRA8_UNORM) {
        formats.push_back(render::TextureFormat::RGBA8_UNORM);
    } else if (RequestedFormat == render::TextureFormat::RGBA8_UNORM) {
        formats.push_back(render::TextureFormat::BGRA8_UNORM);
    }

    Swapchain.reset();
    for (render::TextureFormat format : formats) {
        if (format == render::TextureFormat::UNKNOWN) {
            continue;
        }
        render::SwapChainDescriptor swapchainDesc{};
        swapchainDesc.PresentQueue = Queue;
        swapchainDesc.NativeHandler = NativeHandler;
        swapchainDesc.Width = Width;
        swapchainDesc.Height = Height;
        swapchainDesc.BackBufferCount = BackBufferCount;
        swapchainDesc.FlightFrameCount = FlightFrameCount;
        swapchainDesc.Format = format;
        swapchainDesc.PresentMode = PresentMode;
        auto swapchainOpt = Device->CreateSwapChain(swapchainDesc);
        if (!swapchainOpt.HasValue()) {
            continue;
        }
        Swapchain = swapchainOpt.Release();
        ActiveFormat = format;
        const uint32_t count = Swapchain->GetBackBufferCount();
        BackBufferStates.assign(count, render::TextureState::Undefined);
        BackBufferRtvs.clear();
        BackBufferRtvs.resize(count);
        BackBufferTargets.assign(count, nullptr);
        return true;
    }
    return StoreReason(reason, "CreateSwapChain failed for the requested formats");
}

render::GraphicsPipelineState* RendererRuntimeImpl::GetOrCreatePipeline(const MeshSubmesh& submesh, Nullable<string*> reason) noexcept {
    const auto keyOpt = BuildPipelineKey(submesh, ActiveFormat);
    if (!keyOpt.has_value()) {
        StoreReason(reason, "unsupported mesh vertex layout for runtime forward pipeline");
        return nullptr;
    }
    auto found = Program.PipelineStates.find(keyOpt.value());
    if (found != Program.PipelineStates.end()) {
        return found->second.get();
    }

    render::ColorTargetState colorTarget = render::ColorTargetState::Default(ActiveFormat);
    render::GraphicsPipelineStateDescriptor psoDesc{};
    psoDesc.RootSig = Program.RootSignature.get();
    psoDesc.VS = render::ShaderEntry{
        .Target = Program.VertexShader.get(),
        .EntryPoint = "VSMain",
    };
    psoDesc.PS = render::ShaderEntry{
        .Target = Program.PixelShader.get(),
        .EntryPoint = "PSMain",
    };
    psoDesc.VertexLayouts = std::span{&submesh.VertexLayout, 1};
    psoDesc.Primitive = render::PrimitiveState{
        .Topology = render::PrimitiveTopology::TriangleList,
        .FaceClockwise = render::FrontFace::CW,
        .Cull = render::CullMode::None,
        .Poly = render::PolygonMode::Fill,
        .StripIndexFormat = std::nullopt,
        .UnclippedDepth = true,
        .Conservative = false,
    };
    psoDesc.MultiSample = render::MultiSampleState::Default();
    psoDesc.ColorTargets = std::span{&colorTarget, 1};

    auto psoOpt = Device->CreateGraphicsPipelineState(psoDesc);
    if (!psoOpt.HasValue()) {
        StoreReason(reason, "CreateGraphicsPipelineState failed for runtime forward pipeline");
        return nullptr;
    }
    auto pso = psoOpt.Release();
    pso->SetDebugName("runtime_forward_pso");
    auto* result = pso.get();
    Program.PipelineStates.emplace(keyOpt.value(), std::move(pso));
    return result;
}

render::DescriptorSet* RendererRuntimeImpl::GetOrCreateMaterialSet(MaterialHandle handle, Nullable<string*> reason) noexcept {
    auto found = Program.MaterialDescriptorSets.find(handle.Value);
    if (found != Program.MaterialDescriptorSets.end()) {
        return found->second.get();
    }

    auto materialOpt = AssetRegistry->ResolveMaterial(handle);
    if (!materialOpt.HasValue()) {
        StoreReason(reason, "material handle could not be resolved");
        return nullptr;
    }
    auto textureOpt = AssetRegistry->ResolveTexture(materialOpt.Get()->Desc.Albedo);
    auto samplerOpt = AssetRegistry->ResolveSampler(materialOpt.Get()->Desc.Sampler);
    if (!textureOpt.HasValue() || !samplerOpt.HasValue()) {
        StoreReason(reason, "material texture or sampler handle could not be resolved");
        return nullptr;
    }

    auto setOpt = Device->CreateDescriptorSet(Program.RootSignature.get(), render::DescriptorSetIndex{1});
    if (!setOpt.HasValue()) {
        StoreReason(reason, "CreateDescriptorSet failed for material set");
        return nullptr;
    }
    auto set = setOpt.Release();
    if (!set->WriteResource(Program.AlbedoId.value(), textureOpt.Get()->DefaultView.get()) ||
        !set->WriteSampler(Program.AlbedoSamplerId.value(), samplerOpt.Get()->SamplerObject.get())) {
        StoreReason(reason, "failed to populate material descriptor set");
        return nullptr;
    }
    set->SetDebugName(fmt::format("runtime_material_set_{}", handle.Value));
    auto* result = set.get();
    Program.MaterialDescriptorSets.emplace(handle.Value, std::move(set));
    return result;
}

vector<PreparedView> RendererRuntimeImpl::PrepareViews(const FrameSnapshot& snapshot) const {
    vector<PreparedView> views{};
    views.reserve(snapshot.Views.size());
    for (const auto& viewRequest : snapshot.Views) {
        views.push_back(PreparedView{
            .ViewId = viewRequest.ViewId,
            .CameraId = viewRequest.CameraId,
            .OutputWidth = viewRequest.OutputWidth,
            .OutputHeight = viewRequest.OutputHeight,
        });
    }
    for (auto& view : views) {
        for (const auto& meshBatch : snapshot.MeshBatches) {
            if (meshBatch.ViewMask != 0 && (meshBatch.ViewMask & 0x1u) == 0u) {
                continue;
            }
            view.DrawItems.push_back(PreparedDrawItem{
                .Mesh = meshBatch.Mesh,
                .Material = meshBatch.Material,
                .SubmeshIndex = meshBatch.SubmeshIndex,
                .LocalToWorld = meshBatch.LocalToWorld,
                .Tint = Eigen::Vector4f::Ones(),
                .SortKeyHigh = meshBatch.SortKeyHigh,
                .SortKeyLow = meshBatch.SortKeyLow,
            });
        }
        std::sort(
            view.DrawItems.begin(),
            view.DrawItems.end(),
            [](const PreparedDrawItem& lhs, const PreparedDrawItem& rhs) noexcept {
                if (lhs.SortKeyHigh != rhs.SortKeyHigh) {
                    return lhs.SortKeyHigh < rhs.SortKeyHigh;
                }
                if (lhs.SortKeyLow != rhs.SortKeyLow) {
                    return lhs.SortKeyLow < rhs.SortKeyLow;
                }
                if (lhs.Material.Value != rhs.Material.Value) {
                    return lhs.Material.Value < rhs.Material.Value;
                }
                if (lhs.Mesh.Value != rhs.Mesh.Value) {
                    return lhs.Mesh.Value < rhs.Mesh.Value;
                }
                return lhs.SubmeshIndex < rhs.SubmeshIndex;
            });
    }
    return views;
}

const CameraRenderData* RendererRuntimeImpl::FindCamera(const FrameSnapshot& snapshot, uint32_t cameraId) const noexcept {
    auto it = std::find_if(
        snapshot.Cameras.begin(),
        snapshot.Cameras.end(),
        [&](const CameraRenderData& camera) noexcept {
            return camera.CameraId == cameraId;
        });
    return it == snapshot.Cameras.end() ? nullptr : &(*it);
}

}  // namespace

class RendererRuntime::Impl : public RendererRuntimeImpl {
};

RendererRuntime::RendererRuntime() noexcept = default;

RendererRuntime::~RendererRuntime() noexcept {
    this->Destroy();
}

bool RendererRuntime::Initialize(const RendererRuntimeCreateDesc& desc, Nullable<string*> reason) noexcept {
    this->Destroy();
    if (desc.Device == nullptr) {
        return StoreReason(reason, "renderer runtime requires a valid device");
    }
    if (desc.NativeHandler == nullptr) {
        return StoreReason(reason, "renderer runtime requires a native window handle");
    }
    if (desc.Width == 0 || desc.Height == 0) {
        return StoreReason(reason, "renderer runtime requires a non-zero window size");
    }

    auto impl = make_unique<Impl>();
    impl->Device = desc.Device;
    impl->Width = desc.Width;
    impl->Height = desc.Height;
    impl->BackBufferCount = desc.BackBufferCount;
    impl->FlightFrameCount = std::max<uint32_t>(1, desc.FlightFrameCount);
    impl->RequestedFormat = desc.Format;
    impl->PresentMode = desc.PresentMode;
    impl->NativeHandler = desc.NativeHandler;

    auto queueOpt = impl->Device->GetCommandQueue(render::QueueType::Direct, 0);
    if (!queueOpt.HasValue()) {
        return StoreReason(reason, "failed to acquire direct queue for renderer runtime");
    }
    impl->Queue = queueOpt.Get();

    if (!impl->CreateSwapchain(reason)) {
        return false;
    }

    impl->Frames.reserve(impl->FlightFrameCount);
    for (uint32_t index = 0; index < impl->FlightFrameCount; ++index) {
        FrameSlot slot{impl->Device.get()};
        slot.PublicState.FrameIndex = index;
        auto cmdOpt = impl->Device->CreateCommandBuffer(impl->Queue);
        if (!cmdOpt.HasValue()) {
            return StoreReason(reason, fmt::format("CreateCommandBuffer failed for frame slot {}", index));
        }
        slot.CommandBuffer = cmdOpt.Release();
        auto fenceOpt = impl->Device->CreateFence();
        if (!fenceOpt.HasValue()) {
            return StoreReason(reason, fmt::format("CreateFence failed for frame slot {}", index));
        }
        slot.Fence = fenceOpt.Release();
        impl->Frames.push_back(std::move(slot));
    }

    impl->AssetRegistry = make_unique<RenderAssetRegistry>(impl->Device.get());
    if (!impl->Uploader.Initialize(impl->Device.get())) {
        return StoreReason(reason, "failed to initialize upload system");
    }
    if (!impl->InitializeProgram(reason)) {
        return false;
    }

    _impl = std::move(impl);
    return true;
}

void RendererRuntime::Destroy() noexcept {
    if (_impl == nullptr) {
        return;
    }
    this->WaitIdle();
    _impl->Uploader.Destroy();
    _impl.reset();
}

bool RendererRuntime::IsValid() const noexcept {
    return _impl != nullptr && _impl->Swapchain != nullptr && _impl->AssetRegistry != nullptr;
}

RenderAssetRegistry& RendererRuntime::Assets() noexcept {
    RADRAY_ASSERT(_impl != nullptr && _impl->AssetRegistry != nullptr);
    return *_impl->AssetRegistry;
}

const RenderAssetRegistry& RendererRuntime::Assets() const noexcept {
    RADRAY_ASSERT(_impl != nullptr && _impl->AssetRegistry != nullptr);
    return *_impl->AssetRegistry;
}

bool RendererRuntime::RenderFrame(const FrameSnapshot& snapshot, Nullable<string*> reason) noexcept {
    if (!this->IsValid()) {
        return StoreReason(reason, "renderer runtime is not initialized");
    }
    if (snapshot.Views.empty() || snapshot.Cameras.empty()) {
        return StoreReason(reason, "frame snapshot must contain at least one camera and one render view");
    }

    const uint32_t slotIndex = static_cast<uint32_t>(_impl->FrameCounter % _impl->Frames.size());
    auto& slot = _impl->Frames[slotIndex];
    slot.Fence->Wait();
    if (slot.Fence->GetCompletedValue() < slot.PublicState.ExpectedFenceValue) {
        return StoreReason(reason, "fence completed value is stale for renderer frame slot");
    }
    _impl->Uploader.ReleaseFrameResources(slot.PublicState);
    slot.CBufferArena.Reset();
    slot.TemporaryBufferViews.clear();
    slot.TemporaryDescriptorSets.clear();

    render::Texture* backBuffer = nullptr;
    render::SwapChainSyncObject* waitToDrawSync = nullptr;
    render::SwapChainSyncObject* readyToPresentSync = nullptr;
    for (uint32_t retry = 0; retry < 120; ++retry) {
        auto acquired = _impl->Swapchain->AcquireNext();
        if (acquired.BackBuffer.HasValue()) {
            backBuffer = acquired.BackBuffer.Get();
            waitToDrawSync = acquired.WaitToDraw;
            readyToPresentSync = acquired.ReadyToPresent;
            break;
        }
    }
    if (backBuffer == nullptr) {
        return StoreReason(reason, "AcquireNext failed for renderer runtime");
    }

    const uint32_t backBufferIndex = _impl->Swapchain->GetCurrentBackBufferIndex();
    if (!_impl->EnsureBackBufferRtv(backBufferIndex, backBuffer, reason)) {
        return false;
    }

    RenderFrameContext context{};
    context.FrameIndex = slotIndex;
    context.BackBufferIndex = backBufferIndex;
    context.Snapshot = &snapshot;
    context.PreparedViews = _impl->PrepareViews(snapshot);
    if (context.PreparedViews.empty()) {
        return StoreReason(reason, "frame snapshot did not produce any prepared views");
    }

    const PreparedView& preparedView = context.PreparedViews.front();
    const CameraRenderData* camera = _impl->FindCamera(snapshot, preparedView.CameraId);
    if (camera == nullptr) {
        return StoreReason(reason, "prepared render view does not resolve to a camera");
    }

    const uint64_t frameCBufferSize = Align(
        sizeof(FrameConstantsCpu),
        std::max<uint64_t>(16, _impl->Device->GetDetail().CBufferAlignment));
    auto frameAlloc = slot.CBufferArena.Allocate(frameCBufferSize);
    if (frameAlloc.Target == nullptr || frameAlloc.Mapped == nullptr) {
        return StoreReason(reason, "failed to allocate frame constant buffer memory");
    }
    std::memset(frameAlloc.Mapped, 0, static_cast<size_t>(frameCBufferSize));
    auto* frameConstants = static_cast<FrameConstantsCpu*>(frameAlloc.Mapped);
    frameConstants->ViewProj = camera->ViewProj;

    render::BufferViewDescriptor frameCbvDesc{};
    frameCbvDesc.Target = frameAlloc.Target;
    frameCbvDesc.Range = render::BufferRange{frameAlloc.Offset, frameCBufferSize};
    frameCbvDesc.Usage = render::BufferViewUsage::CBuffer;
    auto frameCbvOpt = _impl->Device->CreateBufferView(frameCbvDesc);
    if (!frameCbvOpt.HasValue()) {
        return StoreReason(reason, "failed to create frame constant buffer view");
    }
    auto frameCbv = frameCbvOpt.Release();
    frameCbv->SetDebugName(fmt::format("runtime_frame_cbv_{}", slotIndex));

    auto frameSetOpt = _impl->Device->CreateDescriptorSet(_impl->Program.RootSignature.get(), render::DescriptorSetIndex{0});
    if (!frameSetOpt.HasValue()) {
        return StoreReason(reason, "failed to create frame descriptor set");
    }
    auto frameSet = frameSetOpt.Release();
    if (!frameSet->WriteResource(_impl->Program.FrameConstantsId.value(), frameCbv.get())) {
        return StoreReason(reason, "failed to write frame constant descriptor");
    }
    frameSet->SetDebugName(fmt::format("runtime_frame_set_{}", slotIndex));

    slot.TemporaryBufferViews.push_back(std::move(frameCbv));
    slot.TemporaryDescriptorSets.push_back(std::move(frameSet));
    auto* frameDescriptorSet = slot.TemporaryDescriptorSets.back().get();

    auto* cmd = slot.CommandBuffer.get();
    cmd->Begin();
    if (!_impl->Uploader.ProcessPendingUploads(cmd, *_impl->AssetRegistry, slot.PublicState, reason)) {
        return false;
    }

    render::ResourceBarrierDescriptor toRenderTarget = render::BarrierTextureDescriptor{
        .Target = backBuffer,
        .Before = _impl->BackBufferStates[backBufferIndex],
        .After = render::TextureState::RenderTarget,
    };
    cmd->ResourceBarrier(std::span{&toRenderTarget, 1});

    const render::ColorClearValue clearValue{{0.0f, 0.0f, 0.0f, 1.0f}};
    render::ColorAttachment colorAttachment{
        .Target = _impl->BackBufferRtvs[backBufferIndex].get(),
        .Load = render::LoadAction::Clear,
        .Store = render::StoreAction::Store,
        .ClearValue = clearValue,
    };
    render::RenderPassDescriptor renderPassDesc{};
    renderPassDesc.Name = "RuntimeMainScene";
    renderPassDesc.ColorAttachments = std::span{&colorAttachment, 1};
    auto encoderOpt = cmd->BeginRenderPass(renderPassDesc);
    if (!encoderOpt.HasValue()) {
        return StoreReason(reason, "BeginRenderPass failed in renderer runtime");
    }
    auto encoder = encoderOpt.Release();
    encoder->SetViewport(Viewport{
        .X = 0.0f,
        .Y = 0.0f,
        .Width = static_cast<float>(preparedView.OutputWidth),
        .Height = static_cast<float>(preparedView.OutputHeight),
        .MinDepth = 0.0f,
        .MaxDepth = 1.0f,
    });
    encoder->SetScissor(Rect{
        .X = 0,
        .Y = 0,
        .Width = preparedView.OutputWidth,
        .Height = preparedView.OutputHeight,
    });
    encoder->BindRootSignature(_impl->Program.RootSignature.get());
    encoder->BindDescriptorSet(render::DescriptorSetIndex{0}, frameDescriptorSet);

    MaterialHandle lastMaterial{};
    PipelineKey lastPipelineKey{};
    bool hasLastPipelineKey{false};
    for (const auto& drawItem : preparedView.DrawItems) {
        auto meshOpt = _impl->AssetRegistry->ResolveMesh(drawItem.Mesh);
        auto materialOpt = _impl->AssetRegistry->ResolveMaterial(drawItem.Material);
        if (!meshOpt.HasValue() || !materialOpt.HasValue()) {
            continue;
        }
        const auto* mesh = meshOpt.Get();
        if (!mesh->IsUploaded || drawItem.SubmeshIndex >= mesh->Submeshes.size()) {
            continue;
        }
        const auto& submesh = mesh->Submeshes[drawItem.SubmeshIndex];
        auto* materialSet = _impl->GetOrCreateMaterialSet(drawItem.Material, reason);
        if (materialSet == nullptr) {
            return false;
        }
        const auto keyOpt = BuildPipelineKey(submesh, _impl->ActiveFormat);
        if (!keyOpt.has_value()) {
            continue;
        }
        if (!hasLastPipelineKey || keyOpt.value() != lastPipelineKey) {
            auto* pso = _impl->GetOrCreatePipeline(submesh, reason);
            if (pso == nullptr) {
                return false;
            }
            encoder->BindGraphicsPipelineState(pso);
            lastPipelineKey = keyOpt.value();
            hasLastPipelineKey = true;
        }
        if (!lastMaterial.IsValid() || lastMaterial != drawItem.Material) {
            encoder->BindDescriptorSet(render::DescriptorSetIndex{1}, materialSet);
            lastMaterial = drawItem.Material;
        }
        encoder->BindVertexBuffer(std::span{&submesh.VertexBuffer, 1});
        encoder->BindIndexBuffer(submesh.IndexBuffer);
        DrawConstantsCpu drawConstants{};
        drawConstants.LocalToWorld = drawItem.LocalToWorld;
        drawConstants.Tint = materialOpt.Get()->Desc.Tint.cwiseProduct(drawItem.Tint);
        encoder->PushConstants(_impl->Program.DrawConstantsId.value(), &drawConstants, sizeof(drawConstants));
        encoder->DrawIndexed(submesh.IndexCount, 1, 0, 0, 0);
    }
    cmd->EndRenderPass(std::move(encoder));

    if (_impl->CaptureEnabled) {
        const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(_impl->ActiveFormat);
        const uint64_t rowPitch = Align(
            static_cast<uint64_t>(_impl->Width) * bytesPerPixel,
            std::max<uint64_t>(1, _impl->Device->GetDetail().TextureDataPitchAlignment));
        const uint64_t captureSize = rowPitch * _impl->Height;
        if (slot.CaptureReadbackBuffer == nullptr || slot.CaptureSize != captureSize) {
            render::BufferDescriptor captureDesc{};
            captureDesc.Size = captureSize;
            captureDesc.Memory = render::MemoryType::ReadBack;
            captureDesc.Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead;
            auto captureOpt = _impl->Device->CreateBuffer(captureDesc);
            if (!captureOpt.HasValue()) {
                return StoreReason(reason, "failed to create capture readback buffer");
            }
            slot.CaptureReadbackBuffer = captureOpt.Release();
            slot.CaptureReadbackBuffer->SetDebugName(fmt::format("runtime_capture_{}", slotIndex));
        }
        slot.CaptureRowPitch = rowPitch;
        slot.CaptureSize = captureSize;
        slot.CaptureWidth = _impl->Width;
        slot.CaptureHeight = _impl->Height;
        slot.CaptureFormat = _impl->ActiveFormat;

        vector<render::ResourceBarrierDescriptor> preCopy{};
        AppendReadbackPreCopyBarrier(_impl->Device.get(), preCopy, slot.CaptureReadbackBuffer.get());
        preCopy.push_back(render::BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = render::TextureState::RenderTarget,
            .After = render::TextureState::CopySource,
        });
        cmd->ResourceBarrier(preCopy);
        cmd->CopyTextureToBuffer(slot.CaptureReadbackBuffer.get(), 0, backBuffer, render::SubresourceRange{0, 1, 0, 1});

        vector<render::ResourceBarrierDescriptor> postCopy{};
        AppendReadbackPostCopyBarrier(_impl->Device.get(), postCopy, slot.CaptureReadbackBuffer.get());
        postCopy.push_back(render::BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = render::TextureState::CopySource,
            .After = render::TextureState::Present,
        });
        cmd->ResourceBarrier(postCopy);
        _impl->LastCaptureSlot = slotIndex;
    } else {
        render::ResourceBarrierDescriptor toPresent = render::BarrierTextureDescriptor{
            .Target = backBuffer,
            .Before = render::TextureState::RenderTarget,
            .After = render::TextureState::Present,
        };
        cmd->ResourceBarrier(std::span{&toPresent, 1});
    }
    cmd->End();

    render::CommandBuffer* submitCmds[] = {cmd};
    render::CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = submitCmds;
    submitDesc.SignalFence = slot.Fence.get();
    submitDesc.WaitToExecute = waitToDrawSync;
    submitDesc.ReadyToPresent = readyToPresentSync;
    _impl->Queue->Submit(submitDesc);
    _impl->Swapchain->Present(readyToPresentSync);

    _impl->BackBufferStates[backBufferIndex] = render::TextureState::Present;
    slot.PublicState.ExpectedFenceValue++;
    _impl->FrameCounter++;
    return true;
}

void RendererRuntime::Resize(uint32_t width, uint32_t height) noexcept {
    if (!this->IsValid() || width == 0 || height == 0) {
        return;
    }
    _impl->Width = width;
    _impl->Height = height;
    this->WaitIdle();
    string reason{};
    if (!_impl->CreateSwapchain(&reason)) {
        RADRAY_ERR_LOG("RendererRuntime::Resize failed: {}", reason);
    }
}

void RendererRuntime::WaitIdle() noexcept {
    if (_impl == nullptr) {
        return;
    }
    if (_impl->Queue != nullptr) {
        _impl->Queue->Wait();
    }
    for (auto& slot : _impl->Frames) {
        if (slot.Fence != nullptr) {
            slot.Fence->Wait();
        }
        _impl->Uploader.ReleaseFrameResources(slot.PublicState);
    }
}

void RendererRuntime::SetCaptureEnabled(bool enabled) noexcept {
    if (_impl == nullptr) {
        return;
    }
    _impl->CaptureEnabled = enabled;
    if (!enabled) {
        _impl->LastCaptureSlot.reset();
    }
}

std::optional<uint32_t> RendererRuntime::ReadCapturedPixel(
    uint32_t x,
    uint32_t y,
    Nullable<string*> reason) const noexcept {
    if (_impl == nullptr) {
        StoreReason(reason, "renderer runtime is not initialized");
        return std::nullopt;
    }
    if (!_impl->LastCaptureSlot.has_value()) {
        StoreReason(reason, "no captured frame is available");
        return std::nullopt;
    }

    const uint32_t slotIndex = _impl->LastCaptureSlot.value();
    if (slotIndex >= _impl->Frames.size()) {
        StoreReason(reason, "captured frame slot index is out of range");
        return std::nullopt;
    }

    const auto& slot = _impl->Frames[slotIndex];
    if (slot.CaptureReadbackBuffer == nullptr || slot.CaptureWidth == 0 || slot.CaptureHeight == 0) {
        StoreReason(reason, "capture readback buffer is not initialized");
        return std::nullopt;
    }
    if (x >= slot.CaptureWidth || y >= slot.CaptureHeight) {
        StoreReason(reason, "capture pixel coordinate is out of bounds");
        return std::nullopt;
    }

    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(slot.CaptureFormat);
    if (bytesPerPixel != 4) {
        StoreReason(reason, "capture format is unsupported for pixel readback");
        return std::nullopt;
    }

    if (slot.Fence != nullptr) {
        slot.Fence->Wait();
    }
    const uint64_t offset = slot.CaptureRowPitch * y + static_cast<uint64_t>(x) * bytesPerPixel;
    if (offset + bytesPerPixel > slot.CaptureSize) {
        StoreReason(reason, "capture pixel offset exceeds readback buffer size");
        return std::nullopt;
    }

    void* mapped = slot.CaptureReadbackBuffer->Map(offset, bytesPerPixel);
    if (mapped == nullptr) {
        StoreReason(reason, "failed to map capture readback buffer");
        return std::nullopt;
    }

    std::array<uint8_t, 4> texel{};
    std::memcpy(texel.data(), mapped, texel.size());
    slot.CaptureReadbackBuffer->Unmap(offset, bytesPerPixel);

    uint8_t red = texel[0];
    uint8_t green = texel[1];
    uint8_t blue = texel[2];
    const uint8_t alpha = texel[3];
    if (slot.CaptureFormat == render::TextureFormat::BGRA8_UNORM) {
        std::swap(red, blue);
    } else if (slot.CaptureFormat != render::TextureFormat::RGBA8_UNORM) {
        StoreReason(reason, "capture format is unsupported for byte order conversion");
        return std::nullopt;
    }

    return (static_cast<uint32_t>(alpha) << 24) |
           (static_cast<uint32_t>(red) << 16) |
           (static_cast<uint32_t>(green) << 8) |
           static_cast<uint32_t>(blue);
}

}  // namespace radray::runtime
