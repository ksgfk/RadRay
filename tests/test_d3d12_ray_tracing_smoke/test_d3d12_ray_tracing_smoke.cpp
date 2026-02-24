#include <gtest/gtest.h>

#include <cstring>

#include <radray/render/common.h>
#include <radray/render/dxc.h>
#include <radray/render/backend/d3d12_impl.h>

using namespace radray;
using namespace radray::render;

static constexpr const char* RT_SHADER = R"(
struct Payload {
    float T;
};

[shader("raygeneration")]
void RayGen() {
}

[shader("miss")]
void Miss(inout Payload payload) {
    payload.T = -1.0;
}

[shader("closesthit")]
void CHS(inout Payload payload, in BuiltInTriangleIntersectionAttributes attr) {
    payload.T = attr.barycentrics.x + attr.barycentrics.y;
}
)";

TEST(D3D12RayTracing, BasicBuildAndTrace) {
    D3D12DeviceDescriptor deviceDesc{};
    auto deviceOpt = CreateDevice(deviceDesc);
    ASSERT_TRUE(deviceOpt.HasValue());
    auto device = deviceOpt.Unwrap();
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(device->GetBackend(), RenderBackend::D3D12);

    const auto detail = device->GetDetail();
    if (!detail.IsRayTracingSupported) {
        GTEST_SKIP() << "Ray tracing is not supported on current device";
    }

    auto dxcOpt = CreateDxc();
    ASSERT_TRUE(dxcOpt.HasValue());
    auto dxc = dxcOpt.Unwrap();

    auto rayGen = dxc->Compile(RT_SHADER, "RayGen", ShaderStage::RayGen, HlslShaderModel::SM66, true);
    auto miss = dxc->Compile(RT_SHADER, "Miss", ShaderStage::Miss, HlslShaderModel::SM66, true);
    auto chs = dxc->Compile(RT_SHADER, "CHS", ShaderStage::ClosestHit, HlslShaderModel::SM66, true);
    ASSERT_TRUE(rayGen.has_value());
    ASSERT_TRUE(miss.has_value());
    ASSERT_TRUE(chs.has_value());

    auto rayGenShaderOpt = device->CreateShader({rayGen->Data, rayGen->Category});
    auto missShaderOpt = device->CreateShader({miss->Data, miss->Category});
    auto chsShaderOpt = device->CreateShader({chs->Data, chs->Category});
    ASSERT_TRUE(rayGenShaderOpt.HasValue());
    ASSERT_TRUE(missShaderOpt.HasValue());
    ASSERT_TRUE(chsShaderOpt.HasValue());
    auto rayGenShader = rayGenShaderOpt.Unwrap();
    auto missShader = missShaderOpt.Unwrap();
    auto chsShader = chsShaderOpt.Unwrap();

    RootSignatureDescriptor rsDesc{};
    auto rootSigOpt = device->CreateRootSignature(rsDesc);
    ASSERT_TRUE(rootSigOpt.HasValue());
    auto rootSig = rootSigOpt.Unwrap();

    RayTracingShaderEntry entries[] = {
        {rayGenShader.get(), "RayGen", ShaderStage::RayGen},
        {missShader.get(), "Miss", ShaderStage::Miss},
        {chsShader.get(), "CHS", ShaderStage::ClosestHit},
    };
    RayTracingHitGroupDescriptor hitGroups[] = {
        {
            "MainHitGroup",
            RayTracingShaderEntry{chsShader.get(), "CHS", ShaderStage::ClosestHit},
            std::nullopt,
            std::nullopt,
        },
    };
    RayTracingPipelineStateDescriptor rtPsoDesc{};
    rtPsoDesc.RootSig = rootSig.get();
    rtPsoDesc.ShaderEntries = entries;
    rtPsoDesc.HitGroups = hitGroups;
    rtPsoDesc.MaxRecursionDepth = 1;
    rtPsoDesc.MaxPayloadSize = 4;
    rtPsoDesc.MaxAttributeSize = 8;

    auto rtPsoOpt = device->CreateRayTracingPipelineState(rtPsoDesc);
    ASSERT_TRUE(rtPsoOpt.HasValue());
    auto rtPso = rtPsoOpt.Unwrap();

    BufferDescriptor vbDesc{};
    vbDesc.Size = sizeof(float) * 9;
    vbDesc.Memory = MemoryType::Upload;
    vbDesc.Usage = BufferUse::Resource | BufferUse::MapWrite;
    auto vbOpt = device->CreateBuffer(vbDesc);
    ASSERT_TRUE(vbOpt.HasValue());
    auto vb = vbOpt.Unwrap();
    {
        float tri[] = {
            0.0f, 0.5f, 0.0f,
            0.5f, -0.5f, 0.0f,
            -0.5f, -0.5f, 0.0f,
        };
        void* mapped = vb->Map(0, sizeof(tri));
        std::memcpy(mapped, tri, sizeof(tri));
        vb->Unmap(0, sizeof(tri));
    }

    BufferDescriptor scratchDesc{};
    scratchDesc.Size = 32ull * 1024 * 1024;
    scratchDesc.Memory = MemoryType::Device;
    scratchDesc.Usage = BufferUse::Scratch;
    auto scratchOpt = device->CreateBuffer(scratchDesc);
    ASSERT_TRUE(scratchOpt.HasValue());
    auto scratch = scratchOpt.Unwrap();

    AccelerationStructureDescriptor blasDesc{};
    blasDesc.Type = AccelerationStructureType::BottomLevel;
    blasDesc.MaxGeometryCount = 1;
    auto blasOpt = device->CreateAccelerationStructure(blasDesc);
    ASSERT_TRUE(blasOpt.HasValue());
    auto blas = blasOpt.Unwrap();

    AccelerationStructureDescriptor tlasDesc{};
    tlasDesc.Type = AccelerationStructureType::TopLevel;
    tlasDesc.MaxInstanceCount = 1;
    auto tlasOpt = device->CreateAccelerationStructure(tlasDesc);
    ASSERT_TRUE(tlasOpt.HasValue());
    auto tlas = tlasOpt.Unwrap();

    const uint64_t sbtStride = Align<uint64_t>(D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES, detail.ShaderTableAlignment == 0 ? 64u : detail.ShaderTableAlignment);
    const uint64_t sbtSize = sbtStride * 3;
    BufferDescriptor sbtDesc{};
    sbtDesc.Size = sbtSize;
    sbtDesc.Memory = MemoryType::Upload;
    sbtDesc.Usage = BufferUse::ShaderTable | BufferUse::MapWrite;
    auto sbtOpt = device->CreateBuffer(sbtDesc);
    ASSERT_TRUE(sbtOpt.HasValue());
    auto sbt = sbtOpt.Unwrap();

    auto* rtPsoD3D12 = d3d12::CastD3D12Object(rtPso.get());
    auto itRayGen = rtPsoD3D12->_shaderIdentifiers.find("RayGen");
    auto itMiss = rtPsoD3D12->_shaderIdentifiers.find("Miss");
    auto itHit = rtPsoD3D12->_shaderIdentifiers.find("MainHitGroup");
    ASSERT_TRUE(itRayGen != rtPsoD3D12->_shaderIdentifiers.end());
    ASSERT_TRUE(itMiss != rtPsoD3D12->_shaderIdentifiers.end());
    ASSERT_TRUE(itHit != rtPsoD3D12->_shaderIdentifiers.end());
    {
        void* mapped = sbt->Map(0, sbtSize);
        std::memset(mapped, 0, sbtSize);
        std::memcpy(static_cast<byte*>(mapped) + 0 * sbtStride, itRayGen->second.data(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        std::memcpy(static_cast<byte*>(mapped) + 1 * sbtStride, itMiss->second.data(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        std::memcpy(static_cast<byte*>(mapped) + 2 * sbtStride, itHit->second.data(), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);
        sbt->Unmap(0, sbtSize);
    }

    auto queue = device->GetCommandQueue(QueueType::Direct, 0);
    ASSERT_NE(queue, nullptr);
    auto cmdOpt = device->CreateCommandBuffer(queue);
    ASSERT_TRUE(cmdOpt.HasValue());
    auto cmd = cmdOpt.Unwrap();

    cmd->Begin();

    RayTracingTrianglesDesc tri{};
    tri.VertexBuffer = vb.get();
    tri.VertexStride = sizeof(float) * 3;
    tri.VertexCount = 3;
    tri.VertexFmt = VertexFormat::FLOAT32X3;

    RayTracingGeometryDesc geom{};
    geom.Geometry = tri;

    BuildBottomLevelASDescriptor buildBlas{};
    buildBlas.Target = blas.get();
    buildBlas.Geometries = std::span<const RayTracingGeometryDesc>(&geom, 1);
    buildBlas.ScratchBuffer = scratch.get();
    buildBlas.ScratchSize = scratchDesc.Size;

    RayTracingInstanceDesc instance{};
    instance.Blas = blas.get();
    BuildTopLevelASDescriptor buildTlas{};
    buildTlas.Target = tlas.get();
    buildTlas.Instances = std::span<const RayTracingInstanceDesc>(&instance, 1);
    buildTlas.ScratchBuffer = scratch.get();
    buildTlas.ScratchSize = scratchDesc.Size;

    auto rtPassOpt = cmd->BeginRayTracingPass();
    ASSERT_TRUE(rtPassOpt.HasValue());
    auto rtPass = rtPassOpt.Unwrap();
    rtPass->BindRootSignature(rootSig.get());
    rtPass->BindRayTracingPipelineState(rtPso.get());
    rtPass->BuildBottomLevelAS(buildBlas);
    rtPass->BuildTopLevelAS(buildTlas);

    TraceRaysDescriptor trace{};
    trace.RayGen = ShaderBindingTableRegion{sbt.get(), 0 * sbtStride, sbtStride, sbtStride};
    trace.Miss = ShaderBindingTableRegion{sbt.get(), 1 * sbtStride, sbtStride, sbtStride};
    trace.HitGroup = ShaderBindingTableRegion{sbt.get(), 2 * sbtStride, sbtStride, sbtStride};
    trace.Width = 1;
    trace.Height = 1;
    trace.Depth = 1;
    rtPass->TraceRays(trace);

    cmd->EndRayTracingPass(std::move(rtPass));
    cmd->End();

    CommandBuffer* cmdRaw[] = {cmd.get()};
    CommandQueueSubmitDescriptor submit{};
    submit.CmdBuffers = cmdRaw;
    queue->Submit(submit);
    queue->Wait();
}
