#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <radray/logger.h>
#include <radray/basic_math.h>
#include <radray/stopwatch.h>
#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/window/native_window.h>
#include <radray/render/backend/d3d12_impl.h>

using namespace radray;
using namespace radray::render;

constexpr int WIN_WIDTH = 1280;
constexpr int WIN_HEIGHT = 720;
constexpr int BACK_BUFFER_COUNT = 3;
constexpr int INFLIGHT_FRAME_COUNT = 2;
const char* RADRAY_APPNAME = "hello_world_rt_d3d12";

const char* RT_SHADER_SRC = R"(
RaytracingAccelerationStructure g_TLAS : register(t0, space1);
RWTexture2D<float4> g_Output : register(u0);
cbuffer RtParams : register(b0) {
    float4 g_BackgroundColor;
};

struct Payload {
    float3 color;
};

[shader("miss")]
void MissMain(inout Payload payload) {
    payload.color = g_BackgroundColor.rgb;
}

[shader("closesthit")]
void ClosestHitMain(inout Payload payload, in BuiltInTriangleIntersectionAttributes attr) {
    // Vertex order: left, top, right
    float wLeft = 1.0 - attr.barycentrics.x - attr.barycentrics.y;
    float wTop = attr.barycentrics.x;
    float wRight = attr.barycentrics.y;
    float3 cLeft = float3(0.0, 1.0, 0.0);
    float3 cTop = float3(1.0, 0.0, 0.0);
    float3 cRight = float3(0.0, 0.0, 1.0);
    payload.color = wLeft * cLeft + wTop * cTop + wRight * cRight;
}

[shader("raygeneration")]
void RayGenMain() {
    uint2 idx = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    float2 uv = ((float2(idx) + 0.5) / float2(dim)) * 2.0 - 1.0;

    RayDesc ray;
    ray.Origin = float3(0.0, 0.0, -1.5);
    ray.Direction = normalize(float3(uv, 1.5));
    ray.TMin = 0.001;
    ray.TMax = 10000.0;

    Payload payload;
    payload.color = float3(0.0, 0.0, 0.0);
    TraceRay(g_TLAS, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);

    g_Output[idx] = float4(payload.color, 1.0);
}
)";

const char* BLIT_SHADER_SRC = R"(
Texture2D<float4> g_Input : register(t0);
SamplerState g_Sampler : register(s0);

struct V2P {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

V2P VSMain(uint vertexId : SV_VertexID) {
    const float2 pos[3] = {
        float2(-1.0, -1.0),
        float2(-1.0, 3.0),
        float2(3.0, -1.0)
    };
    V2P o;
    o.pos = float4(pos[vertexId], 0.0, 1.0);
    o.uv = pos[vertexId] * 0.5 + 0.5;
    return o;
}

float4 PSMain(V2P i) : SV_Target {
    return g_Input.SampleLevel(g_Sampler, i.uv, 0.0);
}
)";

class Frame {
public:
    unique_ptr<d3d12::CmdListD3D12> cmdBuffer;
    unique_ptr<d3d12::FenceD3D12> execFence;
};

shared_ptr<Dxc> dxc;
Eigen::Vector2i winSize;
unique_ptr<NativeWindow> window;
shared_ptr<d3d12::DeviceD3D12> device;
d3d12::CmdQueueD3D12* cmdQueue = nullptr;
unique_ptr<d3d12::SwapChainD3D12> swapchain;
vector<Frame> frames;
vector<unique_ptr<d3d12::TextureViewD3D12>> swapchainRtViews;
vector<unique_ptr<d3d12::TextureD3D12>> rtOutputs;
vector<unique_ptr<d3d12::TextureViewD3D12>> rtOutputUavs;
vector<unique_ptr<d3d12::TextureViewD3D12>> rtOutputSrvs;
vector<unique_ptr<d3d12::GpuDescriptorHeapViews>> rtDescSets;
vector<unique_ptr<d3d12::GpuDescriptorHeapViews>> blitDescSets;
vector<TextureState> rtOutputStates;

unique_ptr<d3d12::RootSigD3D12> rtRootSig;
unique_ptr<d3d12::RayTracingPsoD3D12> rtPso;
unique_ptr<d3d12::BufferD3D12> shaderTableBuf;
uint64_t shaderRecordStride = 0;

unique_ptr<d3d12::RootSigD3D12> blitRootSig;
unique_ptr<d3d12::GraphicsPsoD3D12> blitPso;

unique_ptr<d3d12::BufferD3D12> vertBuf;
unique_ptr<d3d12::BufferD3D12> idxBuf;
unique_ptr<d3d12::BufferD3D12> asScratchBuf;
unique_ptr<d3d12::AccelerationStructureD3D12> blas;
unique_ptr<d3d12::AccelerationStructureD3D12> tlas;
unique_ptr<d3d12::AccelerationStructureViewD3D12> tlasView;

sigslot::connection resizedConn;

void CreateSwapChain() {
    SwapChainDescriptor desc{};
    desc.PresentQueue = cmdQueue;
    desc.NativeHandler = window->GetNativeHandler().Handle;
    desc.Width = static_cast<uint32_t>(winSize.x());
    desc.Height = static_cast<uint32_t>(winSize.y());
    desc.BackBufferCount = BACK_BUFFER_COUNT;
    desc.FlightFrameCount = INFLIGHT_FRAME_COUNT;
    desc.Format = TextureFormat::RGBA8_UNORM;
    desc.PresentMode = PresentMode::Mailbox;
    swapchain = StaticCastUniquePtr<d3d12::SwapChainD3D12>(device->CreateSwapChain(desc).Unwrap());

    swapchainRtViews.clear();
    swapchainRtViews.resize(BACK_BUFFER_COUNT);
}

void RecreatePerBackbufferResources() {
    rtOutputs.clear();
    rtOutputUavs.clear();
    rtOutputSrvs.clear();
    rtDescSets.clear();
    blitDescSets.clear();
    rtOutputStates.clear();

    rtOutputs.reserve(BACK_BUFFER_COUNT);
    rtOutputUavs.reserve(BACK_BUFFER_COUNT);
    rtOutputSrvs.reserve(BACK_BUFFER_COUNT);
    rtDescSets.reserve(BACK_BUFFER_COUNT);
    blitDescSets.reserve(BACK_BUFFER_COUNT);
    rtOutputStates.reserve(BACK_BUFFER_COUNT);

    for (uint32_t i = 0; i < BACK_BUFFER_COUNT; i++) {
        TextureDescriptor texDesc{};
        texDesc.Dim = TextureDimension::Dim2D;
        texDesc.Width = static_cast<uint32_t>(winSize.x());
        texDesc.Height = static_cast<uint32_t>(winSize.y());
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.SampleCount = 1;
        texDesc.Format = TextureFormat::RGBA8_UNORM;
        texDesc.Memory = MemoryType::Device;
        texDesc.Usage = TextureUse::UnorderedAccess | TextureUse::Resource;
        texDesc.Hints = ResourceHint::None;
        texDesc.Name = "rt_output";
        rtOutputs.emplace_back(StaticCastUniquePtr<d3d12::TextureD3D12>(device->CreateTexture(texDesc).Unwrap()));

        TextureViewDescriptor uavDesc{};
        uavDesc.Target = rtOutputs.back().get();
        uavDesc.Dim = TextureDimension::Dim2D;
        uavDesc.Format = TextureFormat::RGBA8_UNORM;
        uavDesc.Range = SubresourceRange::AllSub();
        uavDesc.Usage = TextureViewUsage::UnorderedAccess;
        rtOutputUavs.emplace_back(StaticCastUniquePtr<d3d12::TextureViewD3D12>(device->CreateTextureView(uavDesc).Unwrap()));

        TextureViewDescriptor srvDesc{};
        srvDesc.Target = rtOutputs.back().get();
        srvDesc.Dim = TextureDimension::Dim2D;
        srvDesc.Format = TextureFormat::RGBA8_UNORM;
        srvDesc.Range = SubresourceRange::AllSub();
        srvDesc.Usage = TextureViewUsage::Resource;
        rtOutputSrvs.emplace_back(StaticCastUniquePtr<d3d12::TextureViewD3D12>(device->CreateTextureView(srvDesc).Unwrap()));

        auto rtSet = StaticCastUniquePtr<d3d12::GpuDescriptorHeapViews>(device->CreateDescriptorSet(rtRootSig.get(), 0).Unwrap());
        rtSet->SetResource(0, 0, rtOutputUavs.back().get());
        rtSet->SetResource(1, 0, tlasView.get());
        rtDescSets.emplace_back(std::move(rtSet));

        auto blitSet = StaticCastUniquePtr<d3d12::GpuDescriptorHeapViews>(device->CreateDescriptorSet(blitRootSig.get(), 0).Unwrap());
        blitSet->SetResource(0, 0, rtOutputSrvs.back().get());
        blitDescSets.emplace_back(std::move(blitSet));

        rtOutputStates.push_back(TextureState::Undefined);
    }
}

void OnResized(int width, int height) {
    winSize = {width, height};
    if (!swapchain || width <= 0 || height <= 0) {
        return;
    }
    for (auto& frame : frames) {
        frame.execFence->Wait();
    }
    cmdQueue->Wait();

    swapchainRtViews.clear();
    swapchain.reset();
    CreateSwapChain();
    RecreatePerBackbufferResources();
}

void CreateBlitPipeline() {
    auto vsResult = dxc->Compile(BLIT_SHADER_SRC, "VSMain", ShaderStage::Vertex, HlslShaderModel::SM60, false).value();
    auto psResult = dxc->Compile(BLIT_SHADER_SRC, "PSMain", ShaderStage::Pixel, HlslShaderModel::SM60, false).value();

    ShaderDescriptor vsDesc{};
    vsDesc.Source = vsResult.Data;
    vsDesc.Category = vsResult.Category;
    auto vs = device->CreateShader(vsDesc).Unwrap();

    ShaderDescriptor psDesc{};
    psDesc.Source = psResult.Data;
    psDesc.Category = psResult.Category;
    auto ps = device->CreateShader(psDesc).Unwrap();

    RootSignatureSetElement setElems[] = {
        {0, 0, ResourceBindType::Texture, 1, ShaderStage::Pixel, std::nullopt}};
    RootSignatureDescriptorSet descSet{0, std::span{setElems, 1}};
    SamplerDescriptor samplerDesc{};
    samplerDesc.AddressS = AddressMode::ClampToEdge;
    samplerDesc.AddressT = AddressMode::ClampToEdge;
    samplerDesc.AddressR = AddressMode::ClampToEdge;
    samplerDesc.MinFilter = FilterMode::Linear;
    samplerDesc.MagFilter = FilterMode::Linear;
    samplerDesc.MipmapFilter = FilterMode::Linear;
    RootSignatureStaticSampler staticSampler{0, 0, 0, ShaderStage::Pixel, samplerDesc};

    RootSignatureDescriptor rsDesc{};
    rsDesc.DescriptorSets = std::span{&descSet, 1};
    rsDesc.StaticSamplers = std::span{&staticSampler, 1};
    blitRootSig = StaticCastUniquePtr<d3d12::RootSigD3D12>(device->CreateRootSignature(rsDesc).Unwrap());

    ColorTargetState cts = ColorTargetState::Default(TextureFormat::RGBA8_UNORM);
    GraphicsPipelineStateDescriptor psoDesc{};
    psoDesc.RootSig = blitRootSig.get();
    psoDesc.VS = {vs.get(), "VSMain"};
    psoDesc.PS = {ps.get(), "PSMain"};
    psoDesc.VertexLayouts = {};
    psoDesc.Primitive = PrimitiveState::Default();
    psoDesc.Primitive.Cull = CullMode::None;
    psoDesc.DepthStencil = std::nullopt;
    psoDesc.MultiSample = MultiSampleState::Default();
    psoDesc.ColorTargets = std::span{&cts, 1};
    blitPso = StaticCastUniquePtr<d3d12::GraphicsPsoD3D12>(device->CreateGraphicsPipelineState(psoDesc).Unwrap());
}

void CreateRtPipelineAndSbt() {
    auto rayGenResult = dxc->Compile(RT_SHADER_SRC, "RayGenMain", ShaderStage::RayGen, HlslShaderModel::SM65, false).value();
    auto missResult = dxc->Compile(RT_SHADER_SRC, "MissMain", ShaderStage::Miss, HlslShaderModel::SM65, false).value();
    auto chResult = dxc->Compile(RT_SHADER_SRC, "ClosestHitMain", ShaderStage::ClosestHit, HlslShaderModel::SM65, false).value();

    ShaderDescriptor rayGenDesc{};
    rayGenDesc.Source = rayGenResult.Data;
    rayGenDesc.Category = rayGenResult.Category;
    auto rayGenShader = device->CreateShader(rayGenDesc).Unwrap();

    ShaderDescriptor missDesc{};
    missDesc.Source = missResult.Data;
    missDesc.Category = missResult.Category;
    auto missShader = device->CreateShader(missDesc).Unwrap();

    ShaderDescriptor chDesc{};
    chDesc.Source = chResult.Data;
    chDesc.Category = chResult.Category;
    auto chShader = device->CreateShader(chDesc).Unwrap();

    RootSignatureSetElement setElems[] = {
        {0, 0, ResourceBindType::RWTexture, 1, ShaderStage::RayGen, std::nullopt},
        {0, 1, ResourceBindType::AccelerationStructure, 1, ShaderStage::RayGen, std::nullopt}};
    RootSignatureDescriptorSet descSet{0, std::span{setElems, 2}};
    RootSignatureDescriptor rsDesc{};
    rsDesc.DescriptorSets = std::span{&descSet, 1};
    rsDesc.Constant = RootSignatureConstant{0, 0, sizeof(float) * 4, ShaderStage::Miss};
    rtRootSig = StaticCastUniquePtr<d3d12::RootSigD3D12>(device->CreateRootSignature(rsDesc).Unwrap());

    RayTracingShaderEntry shaderEntries[] = {
        {rayGenShader.get(), "RayGenMain", ShaderStage::RayGen},
        {missShader.get(), "MissMain", ShaderStage::Miss},
        {chShader.get(), "ClosestHitMain", ShaderStage::ClosestHit}};
    RayTracingHitGroupDescriptor hitGroup{};
    hitGroup.Name = "HitGroup0";
    hitGroup.ClosestHit = RayTracingShaderEntry{chShader.get(), "ClosestHitMain", ShaderStage::ClosestHit};

    RayTracingPipelineStateDescriptor psoDesc{};
    psoDesc.RootSig = rtRootSig.get();
    psoDesc.ShaderEntries = std::span{shaderEntries, 3};
    psoDesc.HitGroups = std::span{&hitGroup, 1};
    psoDesc.MaxRecursionDepth = 1;
    psoDesc.MaxPayloadSize = sizeof(float) * 3;
    psoDesc.MaxAttributeSize = sizeof(float) * 2;
    rtPso = StaticCastUniquePtr<d3d12::RayTracingPsoD3D12>(device->CreateRayTracingPipelineState(psoDesc).Unwrap());

    auto detail = device->GetDetail();
    shaderRecordStride = Align(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, static_cast<uint64_t>(detail.ShaderTableAlignment));
    const uint64_t sbtSize = shaderRecordStride * 3;
    BufferDescriptor sbtDesc{};
    sbtDesc.Size = sbtSize;
    sbtDesc.Memory = MemoryType::Upload;
    sbtDesc.Usage = BufferUse::ShaderTable | BufferUse::MapWrite;
    sbtDesc.Hints = ResourceHint::None;
    sbtDesc.Name = "rt_sbt";
    shaderTableBuf = StaticCastUniquePtr<d3d12::BufferD3D12>(device->CreateBuffer(sbtDesc).Unwrap());

    auto* pso = static_cast<d3d12::RayTracingPsoD3D12*>(rtPso.get());
    auto& idRayGen = pso->_shaderIdentifiers.at("RayGenMain");
    auto& idMiss = pso->_shaderIdentifiers.at("MissMain");
    auto& idHitGroup = pso->_shaderIdentifiers.at("HitGroup0");

    radray::byte* map = static_cast<radray::byte*>(shaderTableBuf->Map(0, sbtSize));
    std::memset(map, 0, static_cast<size_t>(sbtSize));
    std::memcpy(map + shaderRecordStride * 0, idRayGen.data(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(map + shaderRecordStride * 1, idMiss.data(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    std::memcpy(map + shaderRecordStride * 2, idHitGroup.data(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
    shaderTableBuf->Unmap(0, sbtSize);
}

void CreateGeometryAndAs() {
    // Equilateral triangle in NDC plane, vertex order: left, top, right.
    float pos[] = {
        -0.5f, -0.28867513f, 0.0f,  // left
        0.0f, 0.57735027f, 0.0f,    // top
        0.5f, -0.28867513f, 0.0f    // right
    };
    uint16_t idx[] = {0, 1, 2};
    const uint64_t vertexSize = sizeof(pos);
    const uint64_t indexSize = sizeof(idx);

    auto vertUpload = device->CreateBuffer({vertexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, "vert_upload"}).Unwrap();
    auto idxUpload = device->CreateBuffer({indexSize, MemoryType::Upload, BufferUse::CopySource | BufferUse::MapWrite, ResourceHint::None, "idx_upload"}).Unwrap();

    vertBuf = StaticCastUniquePtr<d3d12::BufferD3D12>(device->CreateBuffer({vertexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Vertex, ResourceHint::None, "vert"}).Unwrap());
    idxBuf = StaticCastUniquePtr<d3d12::BufferD3D12>(device->CreateBuffer({indexSize, MemoryType::Device, BufferUse::CopyDestination | BufferUse::Index, ResourceHint::None, "index"}).Unwrap());

    void* vertMap = vertUpload->Map(0, vertexSize);
    std::memcpy(vertMap, pos, vertexSize);
    vertUpload->Unmap(0, vertexSize);

    void* idxMap = idxUpload->Map(0, indexSize);
    std::memcpy(idxMap, idx, indexSize);
    idxUpload->Unmap(0, indexSize);

    auto uploadCmd = StaticCastUniquePtr<d3d12::CmdListD3D12>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    uploadCmd->Begin();
    {
        ResourceBarrierDescriptor barriers[] = {
            BarrierBufferDescriptor{vertBuf.get(), BufferState::Common, BufferState::CopyDestination, nullptr, false},
            BarrierBufferDescriptor{idxBuf.get(), BufferState::Common, BufferState::CopyDestination, nullptr, false}};
        uploadCmd->ResourceBarrier(barriers);
    }
    uploadCmd->CopyBufferToBuffer(vertBuf.get(), 0, vertUpload.get(), 0, vertexSize);
    uploadCmd->CopyBufferToBuffer(idxBuf.get(), 0, idxUpload.get(), 0, indexSize);
    {
        ResourceBarrierDescriptor barriers[] = {
            BarrierBufferDescriptor{vertBuf.get(), BufferState::CopyDestination, BufferState::AccelerationStructureBuildInput, nullptr, false},
            BarrierBufferDescriptor{idxBuf.get(), BufferState::CopyDestination, BufferState::AccelerationStructureBuildInput, nullptr, false}};
        uploadCmd->ResourceBarrier(barriers);
    }
    uploadCmd->End();
    CommandBuffer* submitCmd[] = {uploadCmd.get()};
    cmdQueue->Submit({submitCmd, {}, {}, {}});
    cmdQueue->Wait();

    auto detail = device->GetDetail();
    const uint64_t scratchSize = Align(8ull << 20, static_cast<uint64_t>(detail.AccelerationStructureScratchAlignment));
    asScratchBuf = StaticCastUniquePtr<d3d12::BufferD3D12>(device->CreateBuffer({scratchSize, MemoryType::Device, BufferUse::Scratch, ResourceHint::None, "as_scratch"}).Unwrap());

    AccelerationStructureDescriptor blasDesc{};
    blasDesc.Type = AccelerationStructureType::BottomLevel;
    blasDesc.MaxGeometryCount = 1;
    blasDesc.Flags = AccelerationStructureBuildFlag::PreferFastTrace;
    blasDesc.Name = "blas";
    blas = StaticCastUniquePtr<d3d12::AccelerationStructureD3D12>(device->CreateAccelerationStructure(blasDesc).Unwrap());

    AccelerationStructureDescriptor tlasDesc{};
    tlasDesc.Type = AccelerationStructureType::TopLevel;
    tlasDesc.MaxInstanceCount = 1;
    tlasDesc.Flags = AccelerationStructureBuildFlag::PreferFastTrace;
    tlasDesc.Name = "tlas";
    tlas = StaticCastUniquePtr<d3d12::AccelerationStructureD3D12>(device->CreateAccelerationStructure(tlasDesc).Unwrap());

    RayTracingTrianglesDescriptor tri{};
    tri.VertexBuffer = vertBuf.get();
    tri.VertexStride = sizeof(float) * 3;
    tri.VertexCount = 3;
    tri.VertexFmt = VertexFormat::FLOAT32X3;
    tri.IndexBuffer = idxBuf.get();
    tri.IndexCount = 3;
    tri.IndexFmt = IndexFormat::UINT16;
    RayTracingGeometryDesc geom{};
    geom.Geometry = tri;
    geom.Opaque = true;

    BuildBottomLevelASDescriptor buildBlas{};
    buildBlas.Target = blas.get();
    buildBlas.Geometries = std::span{&geom, 1};
    buildBlas.ScratchBuffer = asScratchBuf.get();
    buildBlas.ScratchOffset = 0;
    buildBlas.ScratchSize = asScratchBuf->GetDesc().Size;
    buildBlas.Mode = AccelerationStructureBuildMode::Build;

    RayTracingInstanceDescriptor inst{};
    inst.Transform = Eigen::Matrix4f::Identity();
    inst.InstanceID = 0;
    inst.InstanceMask = 0xFF;
    inst.InstanceContributionToHitGroupIndex = 0;
    inst.Blas = blas.get();

    BuildTopLevelASDescriptor buildTlas{};
    buildTlas.Target = tlas.get();
    buildTlas.Instances = std::span{&inst, 1};
    buildTlas.ScratchBuffer = asScratchBuf.get();
    buildTlas.ScratchOffset = 0;
    buildTlas.ScratchSize = asScratchBuf->GetDesc().Size;
    buildTlas.Mode = AccelerationStructureBuildMode::Build;

    auto asCmd = StaticCastUniquePtr<d3d12::CmdListD3D12>(device->CreateCommandBuffer(cmdQueue).Unwrap());
    asCmd->Begin();
    {
        ResourceBarrierDescriptor barriers[] = {
            BarrierBufferDescriptor{asScratchBuf.get(), BufferState::Common, BufferState::AccelerationStructureBuildScratch, nullptr, false},
        };
        asCmd->ResourceBarrier(barriers);
    }
    {
        auto rtPass = asCmd->BeginRayTracingPass().Unwrap();
        rtPass->BuildBottomLevelAS(buildBlas);
        asCmd->EndRayTracingPass(std::move(rtPass));
    }
    {
        ResourceBarrierDescriptor barriers[] = {
            BarrierAccelerationStructureDescriptor{blas.get(), BufferState::AccelerationStructureRead, BufferState::AccelerationStructureRead, nullptr, false}};
        asCmd->ResourceBarrier(barriers);
    }
    {
        auto rtPass = asCmd->BeginRayTracingPass().Unwrap();
        rtPass->BuildTopLevelAS(buildTlas);
        asCmd->EndRayTracingPass(std::move(rtPass));
    }
    asCmd->End();

    CommandBuffer* asSubmitCmd[] = {asCmd.get()};
    cmdQueue->Submit({asSubmitCmd, {}, {}, {}});
    cmdQueue->Wait();

    AccelerationStructureViewDescriptor asViewDesc{};
    asViewDesc.Target = tlas.get();
    asViewDesc.Name = "tlas_view";
    tlasView = StaticCastUniquePtr<d3d12::AccelerationStructureViewD3D12>(device->CreateAccelerationStructureView(asViewDesc).Unwrap());
}

void Init() {
    dxc = CreateDxc().Unwrap();
    winSize = {WIN_WIDTH, WIN_HEIGHT};

#ifdef RADRAY_PLATFORM_WINDOWS
    Win32WindowCreateDescriptor windowDesc{
        RADRAY_APPNAME,
        winSize.x(),
        winSize.y(),
        -1,
        -1,
        true,
        false,
        false,
        {}};
    window = CreateNativeWindow(windowDesc).Unwrap();
#endif
    if (!window) {
        throw std::runtime_error("Failed to create native window");
    }

    D3D12DeviceDescriptor deviceDesc{};
    deviceDesc.IsEnableDebugLayer = true;
    deviceDesc.IsEnableGpuBasedValid = true;
    device = std::static_pointer_cast<d3d12::DeviceD3D12>(CreateDevice(deviceDesc).Unwrap());
    cmdQueue = static_cast<d3d12::CmdQueueD3D12*>(device->GetCommandQueue(QueueType::Direct, 0).Unwrap());

    CreateSwapChain();

    frames.reserve(INFLIGHT_FRAME_COUNT);
    for (int i = 0; i < INFLIGHT_FRAME_COUNT; i++) {
        auto& frame = frames.emplace_back();
        frame.cmdBuffer = StaticCastUniquePtr<d3d12::CmdListD3D12>(device->CreateCommandBuffer(cmdQueue).Unwrap());
        frame.execFence = StaticCastUniquePtr<d3d12::FenceD3D12>(device->CreateFence().Unwrap());
    }

    CreateBlitPipeline();
    CreateRtPipelineAndSbt();
    CreateGeometryAndAs();
    RecreatePerBackbufferResources();

    resizedConn = window->EventResized().connect(OnResized);
}

void Update() {
    uint32_t currentFrame = 0;
    Stopwatch sw = Stopwatch::StartNew();
    while (true) {
        int64_t time = sw.ElapsedMilliseconds() / 2;
        float timeMinus = time / 1000.0f;
        int colorElement = static_cast<int>(timeMinus) % 3;
        float t = timeMinus - std::floor(timeMinus);
        float colorValue = t < 0.5f ? Lerp(0.0f, 1.0f, t * 2.0f) : Lerp(1.0f, 0.0f, (t - 0.5f) * 2.0f);
        Eigen::Vector3f bgColor{0.0f, 0.0f, 0.0f};
        bgColor[colorElement] = colorValue;
        float bgColor4[4] = {bgColor.x(), bgColor.y(), bgColor.z(), 1.0f};

        window->DispatchEvents();
        if (window->ShouldClose()) {
            break;
        }
        if (winSize.x() == 0 || winSize.y() == 0) {
            continue;
        }

        Frame& frame = frames[currentFrame];
        frame.execFence->Wait();

        Texture* backBuffer = swapchain->AcquireNext({}, nullptr).Unwrap();
        uint32_t backBufferIndex = swapchain->GetCurrentBackBufferIndex();

        if (!swapchainRtViews[backBufferIndex]) {
            TextureViewDescriptor rtViewDesc{};
            rtViewDesc.Target = backBuffer;
            rtViewDesc.Dim = TextureDimension::Dim2D;
            rtViewDesc.Format = TextureFormat::RGBA8_UNORM;
            rtViewDesc.Range = SubresourceRange::AllSub();
            rtViewDesc.Usage = TextureViewUsage::RenderTarget;
            swapchainRtViews[backBufferIndex] = StaticCastUniquePtr<d3d12::TextureViewD3D12>(device->CreateTextureView(rtViewDesc).Unwrap());
        }

        auto* cmd = frame.cmdBuffer.get();
        cmd->Begin();

        {
            ResourceBarrierDescriptor barrier[] = {
                BarrierTextureDescriptor{rtOutputs[backBufferIndex].get(), rtOutputStates[backBufferIndex], TextureState::UnorderedAccess}};
            cmd->ResourceBarrier(barrier);
            rtOutputStates[backBufferIndex] = TextureState::UnorderedAccess;
        }

        {
            auto rtPass = cmd->BeginRayTracingPass().Unwrap();
            rtPass->BindRootSignature(rtRootSig.get());
            rtPass->PushConstant(bgColor4, sizeof(bgColor4));
            rtPass->BindDescriptorSet(0, rtDescSets[backBufferIndex].get());
            rtPass->BindRayTracingPipelineState(rtPso.get());

            TraceRaysDescriptor traceDesc{};
            traceDesc.RayGen = {shaderTableBuf.get(), shaderRecordStride * 0, shaderRecordStride, shaderRecordStride};
            traceDesc.Miss = {shaderTableBuf.get(), shaderRecordStride * 1, shaderRecordStride, shaderRecordStride};
            traceDesc.HitGroup = {shaderTableBuf.get(), shaderRecordStride * 2, shaderRecordStride, shaderRecordStride};
            traceDesc.Width = static_cast<uint32_t>(winSize.x());
            traceDesc.Height = static_cast<uint32_t>(winSize.y());
            traceDesc.Depth = 1;
            rtPass->TraceRays(traceDesc);

            cmd->EndRayTracingPass(std::move(rtPass));
        }
        {
            ResourceBarrierDescriptor barriers[] = {
                BarrierTextureDescriptor{backBuffer, TextureState::Undefined, TextureState::RenderTarget},
                BarrierTextureDescriptor{rtOutputs[backBufferIndex].get(), TextureState::UnorderedAccess, TextureState::ShaderRead}};
            cmd->ResourceBarrier(barriers);
            rtOutputStates[backBufferIndex] = TextureState::ShaderRead;
        }
        {
            ColorAttachment colorAttach{};
            colorAttach.Target = swapchainRtViews[backBufferIndex].get();
            colorAttach.Load = LoadAction::Clear;
            colorAttach.Store = StoreAction::Store;
            colorAttach.ClearValue = ColorClearValue{{{0.0f, 0.0f, 0.0f, 1.0f}}};
            RenderPassDescriptor rpDesc{};
            rpDesc.ColorAttachments = std::span{&colorAttach, 1};
            auto pass = cmd->BeginRenderPass(rpDesc).Unwrap();
            pass->BindRootSignature(blitRootSig.get());
            pass->BindGraphicsPipelineState(blitPso.get());
            pass->BindDescriptorSet(0, blitDescSets[backBufferIndex].get());
            pass->SetViewport({0, 0, static_cast<float>(winSize.x()), static_cast<float>(winSize.y()), 0.0f, 1.0f});
            pass->SetScissor({0, 0, static_cast<uint32_t>(winSize.x()), static_cast<uint32_t>(winSize.y())});
            pass->Draw(3, 1, 0, 0);
            cmd->EndRenderPass(std::move(pass));
        }

        {
            ResourceBarrierDescriptor barriers[] = {
                BarrierTextureDescriptor{backBuffer, TextureState::RenderTarget, TextureState::Present}};
            cmd->ResourceBarrier(barriers);
        }

        cmd->End();

        CommandBuffer* submitCmds[] = {cmd};
        CommandQueueSubmitDescriptor submitDesc{};
        submitDesc.CmdBuffers = std::span{submitCmds, 1};
        submitDesc.SignalFence = frame.execFence.get();
        cmdQueue->Submit(submitDesc);
        swapchain->Present({});

        currentFrame = (currentFrame + 1) % frames.size();
    }
}

void End() {
    resizedConn.disconnect();
    for (auto& frame : frames) {
        frame.execFence->Wait();
    }
    cmdQueue->Wait();

    blitDescSets.clear();
    rtDescSets.clear();
    rtOutputSrvs.clear();
    rtOutputUavs.clear();
    rtOutputs.clear();
    swapchainRtViews.clear();

    tlasView.reset();
    tlas.reset();
    blas.reset();
    asScratchBuf.reset();
    idxBuf.reset();
    vertBuf.reset();
    shaderTableBuf.reset();

    blitPso.reset();
    blitRootSig.reset();
    rtPso.reset();
    rtRootSig.reset();

    frames.clear();
    swapchain.reset();
    cmdQueue = nullptr;
    device.reset();
    window.reset();
    dxc.reset();
}

int main() {
    Init();
    Update();
    End();
    return 0;
}
