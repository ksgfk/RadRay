#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

#include <gtest/gtest.h>

#include <radray/basic_math.h>
#include <radray/image_data.h>
#include <radray/render/common.h>
#include <radray/render/debug.h>
#include <radray/render/dxc.h>

using namespace radray;
using namespace radray::render;

constexpr uint32_t kWidth = 256;
constexpr uint32_t kHeight = 256;

const char* kRtShaderSrc = R"(
RaytracingAccelerationStructure g_TLAS : register(t1);
RWTexture2D<float4> g_Output : register(u0);

struct Payload {
    float3 color;
};

[shader("miss")]
void MissMain(inout Payload payload) {
    payload.color = float3(0.0, 0.0, 0.0);
}

[shader("closesthit")]
void ClosestHitMain(inout Payload payload, in BuiltInTriangleIntersectionAttributes attr) {
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

class TestHelloWorldRT final : public OffScreenTestContext {
public:
    using OffScreenTestContext::OffScreenTestContext;

    ~TestHelloWorldRT() noexcept {
        _rtSet.reset();
        _rtUav.reset();
        _tlasView.reset();
        _tlas.reset();
        _blas.reset();
        _asScratchBuf.reset();
        _idxBuf.reset();
        _vertBuf.reset();
        _sbt.reset();
        _rtPso.reset();
        _rtRs.reset();
    }

    void Init(CommandBuffer* cmd, Fence* fence) override {
        auto backend = _device->GetBackend();
        if (backend != RenderBackend::D3D12 && backend != RenderBackend::Vulkan) {
            throw DebugException("test_hello_world_rt only supports D3D12/Vulkan currently");
        }
        CreateRtPipelineAndSbt();
        CreateGeometryAndAs(cmd);

        TextureViewDescriptor uavDesc{};
        uavDesc.Target = _rt.get();
        uavDesc.Dim = TextureDimension::Dim2D;
        uavDesc.Format = TextureFormat::RGBA8_UNORM;
        uavDesc.Range = SubresourceRange::AllSub();
        uavDesc.Usage = TextureViewUsage::UnorderedAccess;
        _rtUav = _device->CreateTextureView(uavDesc).Unwrap();

        _rtSet = _device->CreateDescriptorSet(_rtRs.get(), 0).Unwrap();
        _rtSet->SetResource(0, 0, _rtUav.get());
        _rtSet->SetResource(1, 0, _tlasView.get());
    }

    void ExecutePass(CommandBuffer* cmd, Fence* fence) override {
        // D3D12 requires RT/DS resources from CREATE_NOT_ZEROED heaps to be
        // initialized via Clear/Discard/Copy before other ops (like UAV writes/copy).
        const ColorAttachment clearAttachment{
            _rtv.get(),
            LoadAction::Clear,
            StoreAction::Store,
            ColorClearValue{{0.0f, 0.0f, 0.0f, 1.0f}}};
        const RenderPassDescriptor clearPassDesc{
            std::span{&clearAttachment, 1},
            std::nullopt,
            "init_offscreen_rt"};
        auto clearPass = cmd->BeginRenderPass(clearPassDesc).Unwrap();
        cmd->EndRenderPass(std::move(clearPass));

        ResourceBarrierDescriptor beginRtBarrier[] = {
            BarrierTextureDescriptor{_rt.get(), TextureState::RenderTarget, TextureState::UnorderedAccess},
        };
        cmd->ResourceBarrier(beginRtBarrier);
        _rtState = TextureState::UnorderedAccess;

        auto rtPass = cmd->BeginRayTracingPass().Unwrap();
        rtPass->BindRootSignature(_rtRs.get());
        rtPass->BindDescriptorSet(0, _rtSet.get());
        rtPass->BindRayTracingPipelineState(_rtPso.get());

        TraceRaysDescriptor traceDesc{};
        traceDesc.Sbt = _sbt.get();
        traceDesc.Width = kWidth;
        traceDesc.Height = kHeight;
        traceDesc.Depth = 1;
        rtPass->TraceRays(traceDesc);
        cmd->EndRayTracingPass(std::move(rtPass));
    }

private:
    std::optional<DxcOutput> CompileRtShader(std::string_view entry, ShaderStage stage) {
        vector<string> defines;
        if (_device->GetBackend() == RenderBackend::D3D12) {
            defines.emplace_back("D3D12");
        } else if (_device->GetBackend() == RenderBackend::Vulkan) {
            defines.emplace_back("VULKAN");
        }
        vector<std::string_view> defineViews;
        for (const auto& d : defines) {
            defineViews.emplace_back(d);
        }
        vector<string> includes;
        includes.emplace_back((_projectDir / "shaderlib").generic_string());
        vector<std::string_view> includeViews;
        for (const auto& i : includes) {
            includeViews.emplace_back(i);
        }
        return _dxc->Compile(
            kRtShaderSrc,
            entry,
            stage,
            HlslShaderModel::SM65,
            false,
            defineViews,
            includeViews,
            _device->GetBackend() == RenderBackend::Vulkan);
    }

    void CreateRtPipelineAndSbt() {
        auto rayGenResult = CompileRtShader("RayGenMain", ShaderStage::RayGen).value();
        auto missResult = CompileRtShader("MissMain", ShaderStage::Miss).value();
        auto chResult = CompileRtShader("ClosestHitMain", ShaderStage::ClosestHit).value();

        auto rayGenShader = _device->CreateShader({rayGenResult.Data, rayGenResult.Category}).Unwrap();
        auto missShader = _device->CreateShader({missResult.Data, missResult.Category}).Unwrap();
        auto chShader = _device->CreateShader({chResult.Data, chResult.Category}).Unwrap();

        RootSignatureSetElement setElems[] = {
            {0, 0, ResourceBindType::RWTexture, 1, ShaderStage::RayGen, std::nullopt},
            {1, 0, ResourceBindType::AccelerationStructure, 1, ShaderStage::RayGen, std::nullopt},
        };
        RootSignatureDescriptorSet descSet{0, std::span{setElems, 2}};
        RootSignatureDescriptor rsDesc{};
        rsDesc.DescriptorSets = std::span{&descSet, 1};
        _rtRs = _device->CreateRootSignature(rsDesc).Unwrap();

        RayTracingShaderEntry shaderEntries[] = {
            {rayGenShader.get(), "RayGenMain", ShaderStage::RayGen},
            {missShader.get(), "MissMain", ShaderStage::Miss},
            {chShader.get(), "ClosestHitMain", ShaderStage::ClosestHit},
        };
        RayTracingHitGroupDescriptor hitGroup{};
        hitGroup.Name = "HitGroup0";
        hitGroup.ClosestHit = RayTracingShaderEntry{chShader.get(), "ClosestHitMain", ShaderStage::ClosestHit};

        RayTracingPipelineStateDescriptor psoDesc{};
        psoDesc.RootSig = _rtRs.get();
        psoDesc.ShaderEntries = std::span{shaderEntries, 3};
        psoDesc.HitGroups = std::span{&hitGroup, 1};
        psoDesc.MaxRecursionDepth = 1;
        psoDesc.MaxPayloadSize = sizeof(float) * 3;
        psoDesc.MaxAttributeSize = sizeof(float) * 2;
        _rtPso = _device->CreateRayTracingPipelineState(psoDesc).Unwrap();

        ShaderBindingTableDescriptor sbtDesc{};
        sbtDesc.Pipeline = _rtPso.get();
        sbtDesc.RayGenCount = 1;
        sbtDesc.MissCount = 1;
        sbtDesc.HitGroupCount = 1;
        sbtDesc.Name = "rt_sbt";
        _sbt = _device->CreateShaderBindingTable(sbtDesc).Unwrap();

        ShaderBindingTableBuildEntry sbtEntries[] = {
            {ShaderBindingTableEntryType::RayGen, "RayGenMain", {}, 0},
            {ShaderBindingTableEntryType::Miss, "MissMain", {}, 0},
            {ShaderBindingTableEntryType::HitGroup, "HitGroup0", {}, 0},
        };
        if (!_sbt->Build(sbtEntries)) {
            throw DebugException("failed to build ray tracing shader binding table");
        }
    }
    void CreateGeometryAndAs(CommandBuffer* cmd) {
        constexpr float vertices[] = {
            -0.5f,
            -0.28867513f,
            0.0f,
            0.0f,
            0.57735027f,
            0.0f,
            0.5f,
            -0.28867513f,
            0.0f,
        };
        constexpr uint16_t indices[] = {0, 1, 2};
        _vertBuf = _device->CreateBuffer({sizeof(vertices), MemoryType::Device, BufferUse::CopyDestination | BufferUse::Vertex, ResourceHint::None, "rt_vb"}).Unwrap();
        _idxBuf = _device->CreateBuffer({sizeof(indices), MemoryType::Device, BufferUse::CopyDestination | BufferUse::Index, ResourceHint::None, "rt_ib"}).Unwrap();
        UploadBuffer(_vertBuf.get(), std::as_bytes(std::span{vertices}));
        UploadBuffer(_idxBuf.get(), std::as_bytes(std::span{indices}));
        _asScratchBuf = _device->CreateBuffer(
                                   {Align(8ull << 20, static_cast<uint64_t>(_device->GetDetail().AccelerationStructureScratchAlignment)),
                                    MemoryType::Device,
                                    BufferUse::Scratch,
                                    ResourceHint::None,
                                    "as_scratch"})
                            .Unwrap();
        AccelerationStructureDescriptor blasDesc{};
        blasDesc.Type = AccelerationStructureType::BottomLevel;
        blasDesc.MaxGeometryCount = 1;
        blasDesc.Flags = AccelerationStructureBuildFlag::PreferFastTrace;
        blasDesc.Name = "blas";
        _blas = _device->CreateAccelerationStructure(blasDesc).Unwrap();

        AccelerationStructureDescriptor tlasDesc{};
        tlasDesc.Type = AccelerationStructureType::TopLevel;
        tlasDesc.MaxInstanceCount = 1;
        tlasDesc.Flags = AccelerationStructureBuildFlag::PreferFastTrace;
        tlasDesc.Name = "tlas";
        _tlas = _device->CreateAccelerationStructure(tlasDesc).Unwrap();

        ResourceBarrierDescriptor initBarriers[] = {
            BarrierBufferDescriptor{_vertBuf.get(), BufferState::Common, BufferState::AccelerationStructureBuildInput},
            BarrierBufferDescriptor{_idxBuf.get(), BufferState::Common, BufferState::AccelerationStructureBuildInput},
            BarrierBufferDescriptor{_asScratchBuf.get(), BufferState::Common, BufferState::AccelerationStructureBuildScratch},
        };
        cmd->ResourceBarrier(initBarriers);

        RayTracingTrianglesDescriptor tri{};
        tri.VertexBuffer = _vertBuf.get();
        tri.VertexStride = sizeof(float) * 3;
        tri.VertexCount = 3;
        tri.VertexFmt = VertexFormat::FLOAT32X3;
        tri.IndexBuffer = _idxBuf.get();
        tri.IndexCount = 3;
        tri.IndexFmt = IndexFormat::UINT16;

        RayTracingGeometryDesc geom{};
        geom.Geometry = tri;
        geom.Opaque = true;

        BuildBottomLevelASDescriptor buildBlas{};
        buildBlas.Target = _blas.get();
        buildBlas.Geometries = std::span{&geom, 1};
        buildBlas.ScratchBuffer = _asScratchBuf.get();
        buildBlas.ScratchOffset = 0;
        buildBlas.ScratchSize = _asScratchBuf->GetDesc().Size;
        buildBlas.Mode = AccelerationStructureBuildMode::Build;

        RayTracingInstanceDescriptor inst{};
        inst.Transform = Eigen::Matrix4f::Identity();
        inst.InstanceID = 0;
        inst.InstanceMask = 0xFF;
        inst.InstanceContributionToHitGroupIndex = 0;
        inst.Blas = _blas.get();

        BuildTopLevelASDescriptor buildTlas{};
        buildTlas.Target = _tlas.get();
        buildTlas.Instances = std::span{&inst, 1};
        buildTlas.ScratchBuffer = _asScratchBuf.get();
        buildTlas.ScratchOffset = 0;
        buildTlas.ScratchSize = _asScratchBuf->GetDesc().Size;
        buildTlas.Mode = AccelerationStructureBuildMode::Build;

        {
            auto rtPass = cmd->BeginRayTracingPass().Unwrap();
            rtPass->BuildBottomLevelAS(buildBlas);
            cmd->EndRayTracingPass(std::move(rtPass));
        }

        ResourceBarrierDescriptor blasBarrier[] = {
            BarrierAccelerationStructureDescriptor{_blas.get(), BufferState::AccelerationStructureRead, BufferState::AccelerationStructureRead},
            BarrierBufferDescriptor{_asScratchBuf.get(), BufferState::AccelerationStructureBuildScratch, BufferState::AccelerationStructureBuildScratch},
        };
        cmd->ResourceBarrier(blasBarrier);

        {
            auto rtPass = cmd->BeginRayTracingPass().Unwrap();
            rtPass->BuildTopLevelAS(buildTlas);
            cmd->EndRayTracingPass(std::move(rtPass));
        }

        AccelerationStructureViewDescriptor asViewDesc{};
        asViewDesc.Target = _tlas.get();
        asViewDesc.Name = "tlas_view";
        _tlasView = _device->CreateAccelerationStructureView(asViewDesc).Unwrap();
    }

private:
    unique_ptr<RootSignature> _rtRs{};
    unique_ptr<RayTracingPipelineState> _rtPso{};
    unique_ptr<DescriptorSet> _rtSet{};
    unique_ptr<TextureView> _rtUav{};

    unique_ptr<ShaderBindingTable> _sbt{};

    unique_ptr<Buffer> _vertBuf{};
    unique_ptr<Buffer> _idxBuf{};
    unique_ptr<Buffer> _asScratchBuf{};
    unique_ptr<AccelerationStructure> _blas{};
    unique_ptr<AccelerationStructure> _tlas{};
    unique_ptr<AccelerationStructureView> _tlasView{};
};

void CompareWithBaseline(TestHelloWorldRT& test, const ImageData& actual) {
    auto errors = test.GetCapturedRenderErrors();
    if (!errors.empty()) {
        std::string msg;
        for (size_t i = 0; i < errors.size(); ++i) {
            msg += "[" + std::to_string(i) + "] " + errors[i] + "\n";
        }
        FAIL() << msg;
    }
    auto baselinePath = test._testEnvDir / "baseline.png";
    if (!std::filesystem::exists(baselinePath)) {
        baselinePath = test._projectDir / "tests" / "test_hello_world_rt" / "baseline.png";
    }
    ASSERT_TRUE(std::filesystem::exists(baselinePath)) << "missing baseline: " << baselinePath.string();
    std::ifstream file{baselinePath, std::ios::binary};
    ASSERT_TRUE(file.good()) << "cannot open baseline: " << baselinePath.string();
    auto expectedOpt = ImageData::LoadPNG(file, {.AddAlphaIfRGB = 0xFF, .IsFlipY = false});
    ASSERT_TRUE(expectedOpt.has_value()) << "failed to load baseline: " << baselinePath.string();
    ImageData expected = std::move(expectedOpt.value());
    ASSERT_EQ(actual.Width, expected.Width);
    ASSERT_EQ(actual.Height, expected.Height);
    ASSERT_EQ(actual.Format, expected.Format);
    auto compare = ImageData::CompareImageRGBA8(actual, expected, 1);
    if (compare.MismatchCount != 0u) {
        test.WriteImageComparisonArtifacts(actual, expected, "hello_world_rt");
    }
    ASSERT_EQ(compare.MismatchCount, 0u)
        << "pixel mismatch count=" << compare.MismatchCount
        << ", first mismatch pixel=(" << (compare.FirstMismatchPixel % actual.Width) << ","
        << (compare.FirstMismatchPixel / actual.Width) << ")"
        << ", channel=" << compare.FirstMismatchChannel << ", actual=" << static_cast<uint32_t>(compare.ActualValue)
        << ", expected=" << static_cast<uint32_t>(compare.ExpectedValue)
        << ", artifacts_dir=" << test._testArtifactsDir.string();
}

TEST(HelloWorldRT, D3D12) {
#if !defined(RADRAY_PLATFORM_WINDOWS) || !defined(RADRAY_ENABLE_D3D12)
    GTEST_SKIP() << "D3D12 is not supported on this platform or not enabled.";
#endif
    D3D12DeviceDescriptor desc{std::nullopt, true, true};
    TestHelloWorldRT test{"d3d12", desc, true, {kWidth, kHeight}, TextureFormat::RGBA8_UNORM};
    if (!test._device->GetDetail().IsRayTracingSupported) {
        GTEST_SKIP() << "D3D12 ray tracing is not supported on this device.";
    }
    ImageData actual = test.Run();
    CompareWithBaseline(test, actual);
}

TEST(HelloWorldRT, Vulkan) {
#if !defined(RADRAY_ENABLE_VULKAN)
    GTEST_SKIP() << "Vulkan is not enabled.";
#else
    VulkanCommandQueueDescriptor queues[] = {
        {QueueType::Direct, 1},
    };
    VulkanDeviceDescriptor desc{};
    desc.Queues = std::span{queues, 1};
    TestHelloWorldRT test{"vulkan", desc, true, {kWidth, kHeight}, TextureFormat::RGBA8_UNORM};
    if (!test._device->GetDetail().IsRayTracingSupported) {
        GTEST_SKIP() << "Vulkan ray tracing is not supported on this device.";
    }
    ImageData actual = test.Run();
    CompareWithBaseline(test, actual);
#endif
}
