#include "gpu_test_fixture.h"
#include "known_gpu_issues.h"
#include <filesystem>
#include <fstream>

#if defined(RADRAY_ENABLE_D3D12)
#include <DXProgrammableCapture.h>
#include <radray/scope_guard.h>
#endif

namespace radray::render {
namespace {

class AttachmentOperations : public testing::TestWithParam<int> {
protected:
    test::DeviceContext Context;
    RenderBackend Backend() const { return GetParam() == 0 ? RenderBackend::D3D12 : RenderBackend::Vulkan; }
    void SetUp() override {
        if (!test::TryCreateDevice(Backend(), Context, true, true, {1, 0, 0}, nullptr, GetParam() != 2 && GetParam() != 4, GetParam() < 3)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(Backend()))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
    }
    void TearDown() override { Context.Reset(); }
    unique_ptr<Shader> Stage(std::string_view name, ShaderStage stage) {
        const string entry = stage == ShaderStage::Vertex ? "VSMain" : stage == ShaderStage::Pixel ? "PSMain"
                                                                                                   : "CSMain";
        const auto path = std::filesystem::path{RADRAY_TEST_SHADER_DIR} / "shader_bytecode" /
                          (string{name} + "." + entry + (Backend() == RenderBackend::Vulkan ? ".spirv.bin" : ".dxil.bin"));
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file || file.tellg() <= 0) return {};
        vector<byte> bytes(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return Context.Device->CreateShader({bytes, Backend() == RenderBackend::Vulkan ? ShaderBlobCategory::SPIRV : ShaderBlobCategory::DXIL, stage}).Unwrap();
    }
    unique_ptr<Texture> Texture2D(TextureFormat format, uint32_t samples, TextureUses usage) {
        return Context.Device->CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = samples, .Format = format, .Usage = usage}).Unwrap();
    }
    unique_ptr<TextureView> View(Texture* texture, TextureViewUsage usage, TextureAspects aspects = TextureAspect::Auto) {
        return Context.Device->CreateTextureView({texture, TextureDimension::Dim2D, texture->GetDesc().Format, {0, 1, 0, 1, aspects}, usage}).Unwrap();
    }
    BindingDescriptor Binding(uint32_t id, BindingKind kind, uint32_t reg, ShaderStages stages = ShaderStage::Compute) {
        return {id, kind, 1, stages, D3D12BindingDescriptor{GetBindingRegisterClass(kind), reg, 0}, VulkanBindingDescriptor{0, id}};
    }
    void Barrier(CommandBuffer* command, Texture* texture, TextureStates before, TextureStates after, TextureAspects aspects = TextureAspect::Auto) {
        const ResourceBarrierDescriptor barrier = BarrierTextureDescriptor{texture, before, after, {0, 1, 0, 1, aspects}};
        command->ResourceBarrier(std::span{&barrier, 1});
    }
    void Submit(CommandBuffer* command) {
        command->End();
        CommandBuffer* commands[]{command};
        Context.Queue->Submit({.CmdBuffers = commands});
        Context.Queue->Wait();
    }
    void CheckColorResolve(TextureFormat format, StoreAction store);
};

void AttachmentOperations::CheckColorResolve(TextureFormat format, StoreAction store) {
    auto& device = *Context.Device;
    SCOPED_TRACE(fmt::format("format={} store={}", format, static_cast<uint32_t>(store)));
    if (!device.QueryAttachmentResolveSupport(format, 4).ColorAverage) GTEST_SKIP() << "4x color attachment resolve unavailable";
    auto source = Texture2D(format, 4, TextureUse::RenderTarget | TextureUse::Resource);
    auto target = Texture2D(format, 1, TextureUse::RenderTarget | TextureUse::CopySource);
    auto sourceView = View(source.get(), TextureViewUsage::RenderTarget);
    auto sourceSrv = View(source.get(), TextureViewUsage::Resource);
    auto targetView = View(target.get(), TextureViewUsage::RenderTarget);
    auto vs = Stage("attachment_color", ShaderStage::Vertex);
    auto ps = Stage("attachment_color", ShaderStage::Pixel);
    auto cs = Stage("attachment_read_color", ShaderStage::Compute);
    ASSERT_TRUE(vs && ps && cs);
    const PushConstantDescriptor push{0, 16, ShaderStage::Pixel, PushConstantDescriptor::D3D12Location{0, 0}, PushConstantDescriptor::VulkanLocation{0}};
    auto layout = device.CreatePipelineLayout({{}, std::span{&push, 1}}).Unwrap();
    const auto color = ColorTargetState::Default(format);
    vector<unique_ptr<GraphicsPipelineState>> pipelines;
    for (uint32_t sample = 0; sample < 4; ++sample) {
        auto primitive = PrimitiveState::Default();
        primitive.Cull = CullMode::None;
        pipelines.push_back(device.CreateGraphicsPipelineState({.PipelineLayout = layout.get(), .VS = ShaderEntry{vs.get(), "VSMain"}, .PS = ShaderEntry{ps.get(), "PSMain"}, .Primitive = primitive, .MultiSample = {4, uint64_t{1} << sample, false}, .ColorTargets = std::span{&color, 1}}).Unwrap());
    }
    const BindingDescriptor bindings[]{Binding(0, BindingKind::Texture, 0), Binding(1, BindingKind::RWStructuredBuffer, 0)};
    auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
    const PipelineLayoutGroupDescriptor slot{0, group.get()};
    auto computeLayout = device.CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
    auto compute = device.CreateComputePipelineState({computeLayout.get(), {cs.get(), "CSMain"}}).Unwrap();
    auto output = device.CreateBuffer({.Size = 4096, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
    auto params = device.CreateShaderParameterSet({group.get()}).Unwrap();
    ASSERT_TRUE(params->Set(0, 0, sourceSrv.get()));
    ASSERT_TRUE(params->Set(1, 0, ShaderBufferBinding{output.get(), {0, 4096}, 16}));
    ASSERT_TRUE(params->FlushWrites());
    const auto alignment = device.GetCapabilities().Detail.TextureDataPitchAlignment;
    const auto pixelBytes = format == TextureFormat::RGBA32_FLOAT ? 16u : 4u;
    const auto pitch = (8 * pixelBytes + alignment - 1) / alignment * alignment;
    auto readback = device.CreateBuffer({.Size = 4096 + pitch * 8, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    Barrier(command.get(), source.get(), TextureState::Undefined, TextureState::RenderTarget);
    Barrier(command.get(), target.get(), TextureState::Undefined, TextureState::RenderTarget);
    RenderPassColorAttachment attachment{targetView.get(), LoadAction::Clear, StoreAction::Store, {{0, 1, 0, 1}}};
    auto encoder = command->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    attachment = {sourceView.get(), LoadAction::Clear, StoreAction::Store, {{0, 0, 0, 1}}};
    encoder = command->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    Barrier(command.get(), source.get(), TextureState::RenderTarget, TextureState::RenderTarget);
    Barrier(command.get(), target.get(), TextureState::RenderTarget, TextureState::AttachmentResolveDestination);
    attachment.Load = LoadAction::Load;
    attachment.Store = store;
    attachment.ResolveTarget = targetView.get();
    encoder = command->BeginRenderPass({.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8, .RenderArea = Rect{2, 2, 4, 4}}).Unwrap();
    encoder->SetViewport({0, 0, 8, 8, 0, 1});
    encoder->SetScissor({2, 2, 4, 4});
    for (uint32_t sample = 0; sample < 4; ++sample) {
        const float values[]{float(sample + 1) / 8.0f, 0, 0, 1};
        encoder->BindGraphicsPipelineState(pipelines[sample].get());
        ASSERT_TRUE(encoder->SetPushConstants(0, 0, std::as_bytes(std::span{values})));
        encoder->Draw(3, 1, 0, 0);
    }
    command->EndRenderPass(std::move(encoder));
    Barrier(command.get(), source.get(), TextureState::RenderTarget, TextureState::ShaderRead);
    Barrier(command.get(), target.get(), TextureState::AttachmentResolveDestination, TextureState::CopySource);
    const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{output.get(), BufferState::Undefined, BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&toUav, 1});
    auto read = command->BeginComputePass().Unwrap();
    read->BindComputePipelineState(compute.get());
    read->BindShaderParameterSet(0, params.get(), {});
    read->Dispatch(1, 1, 1);
    command->EndComputePass(std::move(read));
    const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{output.get(), BufferState::UnorderedAccess, BufferState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyBufferToBuffer(readback.get(), 0, output.get(), 0, 4096);
    command->CopyTextureToBuffer(readback.get(), 4096, target.get(), {0, 1, 0, 1});
    test::GpuWorkGuard gpuWork{Context};
    Submit(command.get());
    const auto* data = static_cast<const byte*>(readback->Map(0, 4096 + pitch * 8));
    ASSERT_NE(data, nullptr);
    readback->InvalidateMappedRange({0, 4096 + pitch * 8});
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x) {
            const bool inside = x >= 2 && x < 6 && y >= 2 && y < 6;
            for (uint32_t sample = 0; sample < 4; ++sample) {
                const auto* value = reinterpret_cast<const float*>(data) + ((y * 8 + x) * 4 + sample) * 4;
                if (store == StoreAction::Store || !inside) EXPECT_NEAR(value[0], inside ? float(sample + 1) / 8.0f : 0.0f, 0.006f);
            }
            const auto* resolved = data + 4096 + pitch * y + pixelBytes * x;
            float red, green;
            if (format == TextureFormat::RGBA32_FLOAT) {
                red = reinterpret_cast<const float*>(resolved)[0];
                green = reinterpret_cast<const float*>(resolved)[1];
            } else {
                red = reinterpret_cast<const uint8_t*>(resolved)[0] / 255.0f;
                green = reinterpret_cast<const uint8_t*>(resolved)[1] / 255.0f;
                if (format == TextureFormat::RGBA8_UNORM_SRGB) red = red <= 0.04045f ? red / 12.92f : std::pow((red + 0.055f) / 1.055f, 2.4f);
            }
            EXPECT_NEAR(red, inside ? 0.3125f : 0.0f, 0.006f);
            EXPECT_FLOAT_EQ(green, inside ? 0.0f : 1.0f);
        }
    readback->Unmap();
    allocator->Reset();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_P(AttachmentOperations, PartialColorResolvePreservesTargetOutsideAndEverySourceSample) {
    CheckColorResolve(TextureFormat::RGBA8_UNORM, StoreAction::Store);
}
TEST_P(AttachmentOperations, PartialSrgbResolvePreservesLinearSampleValues) {
    CheckColorResolve(TextureFormat::RGBA8_UNORM_SRGB, StoreAction::Store);
}
TEST_P(AttachmentOperations, PartialFloatResolvePreservesOutsideEvenWithDiscard) {
    CheckColorResolve(TextureFormat::RGBA32_FLOAT, StoreAction::Discard);
}

TEST_P(AttachmentOperations, DepthStencilResolveModesPreserveUntouchedAspectAndOutsideArea) {
    auto& device = *Context.Device;
    const char* stagedReadbackSetting = std::getenv("RADRAY_TEST_DEPTH_RESOLVE_STAGED_READBACK");
    const bool stagedReadback = stagedReadbackSetting && std::string_view{stagedReadbackSetting} == "1";
    RecordProperty("depth_resolve_staged_readback", stagedReadback ? "enabled" : "disabled");
#if defined(RADRAY_ENABLE_D3D12)
    const char* pixCaptureSetting = std::getenv("RADRAY_TEST_DEPTH_RESOLVE_PIX_CAPTURE");
    const bool pixCapture = Backend() == RenderBackend::D3D12 && pixCaptureSetting && std::string_view{pixCaptureSetting} == "1";
    RecordProperty("depth_resolve_pix_capture", pixCapture ? "enabled" : "disabled");
    d3d12::ComPtr<IDXGraphicsAnalysis> graphicsAnalysis;
    if (pixCapture) {
        const auto result = ::DXGIGetDebugInterface1(0, IID_PPV_ARGS(&graphicsAnalysis));
        ASSERT_TRUE(SUCCEEDED(result)) << fmt::format("PIX GPU capture interface unavailable ({:#010x}); launch this test through PIX with GPU Capture enabled", static_cast<uint32_t>(result));
    }
#endif
    constexpr uint64_t kSnapshotBytes = 4096;
    const uint64_t readbackBytes = stagedReadback ? 3 * kSnapshotBytes : kSnapshotBytes;
    const auto format = TextureFormat::D32_FLOAT_S8_UINT;
    const auto support = device.QueryAttachmentResolveSupport(format, 4);
    if (!support.DepthModes) GTEST_SKIP() << "4x depth/stencil attachment resolve unavailable";
    vector<std::pair<ResolveMode, ResolveMode>> modes;
    for (auto depth : {ResolveMode::SampleZero, ResolveMode::Average, ResolveMode::Min, ResolveMode::Max, ResolveMode::None}) {
        for (auto stencil : {ResolveMode::None, ResolveMode::SampleZero, ResolveMode::Min, ResolveMode::Max}) {
            if ((depth != ResolveMode::None && !support.DepthModes.HasFlag(depth)) || (stencil != ResolveMode::None && !support.StencilModes.HasFlag(stencil)) || (depth == ResolveMode::None && stencil == ResolveMode::None)) continue;
            if (depth != stencil && ((depth == ResolveMode::None || stencil == ResolveMode::None) ? !support.IndependentResolveNone : !support.IndependentResolve)) continue;
            modes.emplace_back(depth, stencil);
        }
    }
    ASSERT_FALSE(modes.empty());
    test::GpuTestEnvironment environment;
    ASSERT_TRUE(test::TakeExternal(test::GetGpuTestEnvironment(Context), environment));
    test::KnownGpuIssueSkips skips;
    uint32_t executedModes = 0;
    const TextureDescriptor sourceDescriptor{TextureDimension::Dim2D, 8, 8, 1, 1, 4, format, MemoryType::Device, TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite | TextureUse::Resource};
    auto targetDescriptor = sourceDescriptor;
    targetDescriptor.SampleCount = 1;
    const Rect resolveArea{2, 2, 4, 4};
    for (const auto& [depthMode, stencilMode] : modes) {
        SCOPED_TRACE(fmt::format("depth={}, stencil={}", depthMode, stencilMode));
        if (skips.Add(test::FindDepthResolveKnownIssue(environment, sourceDescriptor, targetDescriptor, resolveArea, depthMode, stencilMode), fmt::format("depth={} stencil={}", depthMode, stencilMode))) continue;
        ++executedModes;
#if defined(RADRAY_ENABLE_D3D12)
        const bool captureThisMode = pixCapture && depthMode == ResolveMode::Average && stencilMode == ResolveMode::None;
        bool captureStarted = false;
        const auto finishCapture = MakeScopeGuard([&] {
            if (captureStarted) {
                Context.Queue->Wait();
                graphicsAnalysis->EndCapture();
            }
        });
#endif
        auto source = device.CreateTexture(sourceDescriptor).Unwrap();
        auto target = device.CreateTexture(targetDescriptor).Unwrap();
        auto sourceView = View(source.get(), TextureViewUsage::DepthWrite);
        auto targetView = View(target.get(), TextureViewUsage::DepthWrite);
        auto sourceDepth = View(source.get(), TextureViewUsage::Resource, TextureAspect::Depth);
        auto sourceStencil = View(source.get(), TextureViewUsage::Resource, TextureAspect::Stencil);
        auto targetDepth = View(target.get(), TextureViewUsage::Resource, TextureAspect::Depth);
        auto targetStencil = View(target.get(), TextureViewUsage::Resource, TextureAspect::Stencil);
        auto vs = Stage("attachment_depth", ShaderStage::Vertex);
        auto cs = Stage("attachment_read_depth", ShaderStage::Compute);
        const PushConstantDescriptor push{0, 16, ShaderStage::Vertex, PushConstantDescriptor::D3D12Location{0, 0}, PushConstantDescriptor::VulkanLocation{0}};
        auto layout = device.CreatePipelineLayout({{}, std::span{&push, 1}}).Unwrap();
        auto primitive = PrimitiveState::Default();
        primitive.Cull = CullMode::None;
        auto ds = DepthStencilState::Default();
        ds.Format = format;
        ds.DepthCompare = CompareFunction::Always;
        ds.Stencil = StencilState::Default();
        ds.Stencil->Front.PassOp = ds.Stencil->Back.PassOp = StencilOperation::IncrementClamp;
        vector<unique_ptr<GraphicsPipelineState>> pipelines;
        for (uint32_t sample = 0; sample < 4; ++sample)
            pipelines.push_back(device.CreateGraphicsPipelineState({.PipelineLayout = layout.get(), .VS = ShaderEntry{vs.get(), "VSMain"}, .Primitive = primitive, .DepthStencil = ds, .MultiSample = {4, uint64_t{1} << sample, false}}).Unwrap());
        const BindingDescriptor bindings[]{Binding(0, BindingKind::Texture, 0), Binding(1, BindingKind::Texture, 1), Binding(2, BindingKind::Texture, 2), Binding(3, BindingKind::Texture, 3), Binding(4, BindingKind::RWStructuredBuffer, 0)};
        auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
        const PipelineLayoutGroupDescriptor slot{0, group.get()};
        auto computeLayout = device.CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
        auto compute = device.CreateComputePipelineState({computeLayout.get(), {cs.get(), "CSMain"}}).Unwrap();
        auto output = device.CreateBuffer({.Size = 4096, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
        auto readback = device.CreateBuffer({.Size = readbackBytes, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
        auto params = device.CreateShaderParameterSet({group.get()}).Unwrap();
        ASSERT_TRUE(params->Set(0, 0, sourceDepth.get()));
        ASSERT_TRUE(params->Set(1, 0, sourceStencil.get()));
        ASSERT_TRUE(params->Set(2, 0, targetDepth.get()));
        ASSERT_TRUE(params->Set(3, 0, targetStencil.get()));
        ASSERT_TRUE(params->Set(4, 0, ShaderBufferBinding{output.get(), {0, 4096}, 16}));
        ASSERT_TRUE(params->FlushWrites());
        auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
        auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
        auto outputState = BufferState::Undefined;
        const auto recordReadback = [&](uint64_t offset) {
            const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{output.get(), outputState, BufferState::UnorderedAccess};
            command->ResourceBarrier(std::span{&toUav, 1});
            auto reader = command->BeginComputePass().Unwrap();
            reader->BindComputePipelineState(compute.get());
            reader->BindShaderParameterSet(0, params.get(), {});
            reader->Dispatch(1, 1, 1);
            command->EndComputePass(std::move(reader));
            const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{output.get(), BufferState::UnorderedAccess, BufferState::CopySource};
            command->ResourceBarrier(std::span{&toCopy, 1});
            command->CopyBufferToBuffer(readback.get(), offset, output.get(), 0, kSnapshotBytes);
            outputState = BufferState::CopySource;
        };
#if defined(RADRAY_ENABLE_D3D12)
        if (captureThisMode) {
            source->SetDebugName("DepthResolveSource4x");
            target->SetDebugName("DepthResolveTarget1x");
            output->SetDebugName("DepthResolveOutput");
            readback->SetDebugName("DepthResolveReadback");
        }
#endif
        command->Begin();
        Barrier(command.get(), source.get(), TextureState::Undefined, TextureState::DepthWrite);
        Barrier(command.get(), target.get(), TextureState::Undefined, TextureState::DepthWrite);
        RenderPassDepthStencilAttachment attachment{targetView.get(), LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store, {0.875f, 19}};
        const auto clearAttachments = [&] {
            attachment.View = targetView.get();
            attachment.Clear = {0.875f, 19};
            auto clear = command->BeginRenderPass({.DepthStencil = attachment, .Width = 8, .Height = 8}).Unwrap();
            command->EndRenderPass(std::move(clear));
            attachment.View = sourceView.get();
            attachment.Clear = {0.75f, 3};
            clear = command->BeginRenderPass({.DepthStencil = attachment, .Width = 8, .Height = 8}).Unwrap();
            command->EndRenderPass(std::move(clear));
        };
        clearAttachments();
#if defined(RADRAY_ENABLE_D3D12)
        if (captureThisMode) {
            Submit(command.get());
            allocator->Reset();
            graphicsAnalysis->BeginCapture();
            captureStarted = true;
            command->Begin();
            clearAttachments();
            RecordProperty("depth_resolve_pix_initialization_submit", "warmup_then_captured_clear");
        }
#endif
        if (stagedReadback) {
            Barrier(command.get(), source.get(), TextureState::DepthWrite, TextureState::ShaderRead);
            Barrier(command.get(), target.get(), TextureState::DepthWrite, TextureState::ShaderRead);
            recordReadback(kSnapshotBytes);
            Barrier(command.get(), source.get(), TextureState::ShaderRead, TextureState::DepthWrite);
            Barrier(command.get(), target.get(), TextureState::ShaderRead, TextureState::DepthWrite);
        }
        Barrier(command.get(), source.get(), TextureState::DepthWrite, TextureState::DepthWrite);
        Barrier(command.get(), target.get(), TextureState::DepthWrite, TextureState::AttachmentResolveDestination);
        attachment.DepthLoad = attachment.StencilLoad = LoadAction::Load;
        attachment.Resolve = DepthStencilResolveDescriptor{targetView.get(), depthMode, stencilMode};
        auto encoder = command->BeginRenderPass({.DepthStencil = attachment, .Width = 8, .Height = 8, .RenderArea = resolveArea}).Unwrap();
        encoder->SetViewport({0, 0, 8, 8, 0, 1});
        encoder->SetScissor({2, 2, 4, 4});
        for (uint32_t sample = 0; sample < 4; ++sample) {
            const float values[]{float(sample + 1) / 8, 0, 0, 0};
            encoder->BindGraphicsPipelineState(pipelines[sample].get());
            ASSERT_TRUE(encoder->SetPushConstants(0, 0, std::as_bytes(std::span{values})));
            for (uint32_t draw = 0; draw <= sample; ++draw) encoder->Draw(3, 1, 0, 0);
        }
        command->EndRenderPass(std::move(encoder));
        Barrier(command.get(), source.get(), TextureState::DepthWrite, TextureState::ShaderRead);
        Barrier(command.get(), target.get(), TextureState::AttachmentResolveDestination, TextureState::ShaderRead);
        if (stagedReadback) recordReadback(2 * kSnapshotBytes);
        recordReadback(0);
        test::GpuWorkGuard gpuWork{Context};
        Submit(command.get());
        auto* data = static_cast<const float*>(readback->Map(0, readbackBytes));
        ASSERT_NE(data, nullptr);
        readback->InvalidateMappedRange({0, readbackBytes});
        if (stagedReadback) {
            const array<std::pair<std::string_view, uint64_t>, 3> snapshots{{{"initialized", kSnapshotBytes}, {"after_end_render_pass", 2 * kSnapshotBytes}, {"final", 0}}};
            for (const auto& [stage, offset] : snapshots) {
                SCOPED_TRACE(stage);
                const auto* snapshot = data + offset / sizeof(float);
                const auto* center = snapshot + (3 * 8 + 3) * 16;
                float insideMin = center[3], insideMax = center[3];
                float outsideMin = snapshot[3], outsideMax = snapshot[3];
                string stencilRows;
                for (uint32_t y = 0; y < 8; ++y) {
                    for (uint32_t x = 0; x < 8; ++x) {
                        const auto* value = snapshot + (y * 8 + x) * 16;
                        const bool inside = x >= 2 && x < 6 && y >= 2 && y < 6;
                        auto& minimum = inside ? insideMin : outsideMin;
                        auto& maximum = inside ? insideMax : outsideMax;
                        minimum = std::min(minimum, value[3]);
                        maximum = std::max(maximum, value[3]);
                        stencilRows += fmt::format("{}{}", x == 0 ? "" : " ", value[3]);
                        if (offset == kSnapshotBytes) {
                            EXPECT_FLOAT_EQ(value[2], 0.875f);
                            EXPECT_FLOAT_EQ(value[3], 19.0f);
                            for (uint32_t sample = 0; sample < 4; ++sample) {
                                EXPECT_FLOAT_EQ(value[sample * 4], 0.75f);
                                EXPECT_FLOAT_EQ(value[sample * 4 + 1], 3.0f);
                            }
                        }
                    }
                    stencilRows += '\n';
                }
                const auto observation = fmt::format("center_depth={} center_stencil={} outside_depth={} outside_stencil={} inside_stencil=[{},{}] outside_stencil=[{},{}] source_stencil=[{},{},{},{}]",
                                                     center[2], center[3], snapshot[2], snapshot[3], insideMin, insideMax, outsideMin, outsideMax, center[1], center[5], center[9], center[13]);
                fmt::print("depth-resolve probe depth={} stencil={} stage={} {}\n", depthMode, stencilMode, stage, observation);
                RecordProperty(fmt::format("resolve_probe_{}_{}_{}", depthMode, stencilMode, stage), fmt::format("{}\nstencil rows:\n{}", observation, stencilRows));
                if (offset == 2 * kSnapshotBytes)
                    EXPECT_TRUE(std::equal(snapshot, snapshot + kSnapshotBytes / sizeof(float), data)) << "EndRenderPass snapshot differs from final readback";
            }
        }
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x)
                for (uint32_t sample = 0; sample < 4; ++sample) {
                    const auto* value = data + ((y * 8 + x) * 4 + sample) * 4;
                    const bool inside = x >= 2 && x < 6 && y >= 2 && y < 6;
                    const auto depth = depthMode == ResolveMode::Average ? 0.3125f : depthMode == ResolveMode::Max ? 0.5f
                                                                                                                   : 0.125f;
                    EXPECT_NEAR(value[0], inside ? float(sample + 1) / 8 : 0.75f, 0.00001f);
                    EXPECT_EQ(value[1], inside ? float(sample + 4) : 3.0f);
                    EXPECT_NEAR(value[2], inside && depthMode != ResolveMode::None ? depth : 0.875f, 0.00001f);
                    EXPECT_EQ(value[3], inside && stencilMode != ResolveMode::None ? (stencilMode == ResolveMode::Max ? 7.0f : 4.0f) : 19.0f);
                }
        readback->Unmap();
        allocator->Reset();
    }
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
    RecordProperty("executed_resolve_modes", executedModes);
    if (skips.Count() && !HasFailure()) GTEST_SKIP() << skips.Summary();
}

TEST_P(AttachmentOperations, ResolveValidationRejectsIncompatibleTargetsModesAndOverlaps) {
    auto& device = *Context.Device;
    auto source = Texture2D(TextureFormat::RGBA8_UNORM, 4, TextureUse::RenderTarget);
    auto target = Texture2D(TextureFormat::RGBA8_UNORM, 1, TextureUse::RenderTarget | TextureUse::Resource);
    auto sourceView = View(source.get(), TextureViewUsage::RenderTarget);
    auto targetView = View(target.get(), TextureViewUsage::RenderTarget);
    auto targetSrv = View(target.get(), TextureViewUsage::Resource);
    RenderPassColorAttachment attachment{sourceView.get(), LoadAction::Load, StoreAction::Store, {}, targetView.get()};
    RenderPassBeginDescriptor pass{.Colors = std::span{&attachment, 1}, .Width = 8, .Height = 8};
    EXPECT_EQ(bool(ValidateRenderScope(pass, device)), device.QueryAttachmentResolveSupport(TextureFormat::RGBA8_UNORM, 4).ColorAverage);
    attachment.ResolveTarget = targetSrv.get();
    EXPECT_FALSE(ValidateRenderScope(pass, device));
    attachment.ResolveTarget = sourceView.get();
    EXPECT_FALSE(ValidateRenderScope(pass, device));
    attachment.View = targetView.get();
    attachment.ResolveTarget = targetView.get();
    EXPECT_FALSE(ValidateRenderScope(pass, device));
    auto incompatible = Texture2D(TextureFormat::RGBA8_UNORM_SRGB, 1, TextureUse::RenderTarget);
    auto incompatibleView = View(incompatible.get(), TextureViewUsage::RenderTarget);
    attachment.View = sourceView.get();
    attachment.ResolveTarget = incompatibleView.get();
    EXPECT_FALSE(ValidateRenderScope(pass, device));
    auto otherSource = Texture2D(TextureFormat::RGBA8_UNORM, 4, TextureUse::RenderTarget);
    auto otherSourceView = View(otherSource.get(), TextureViewUsage::RenderTarget);
    attachment.ResolveTarget = targetView.get();
    RenderPassColorAttachment overlapping[]{attachment, {otherSourceView.get(), LoadAction::Load, StoreAction::Store, {}, targetView.get()}};
    pass.Colors = overlapping;
    EXPECT_FALSE(ValidateRenderScope(pass, device));
    EXPECT_FALSE(device.QueryAttachmentResolveSupport(TextureFormat::RGBA8_UINT, 4).ColorAverage);
    EXPECT_FALSE(device.QueryAttachmentResolveSupport(TextureFormat::RGBA8_UNORM, 1).ColorAverage);
    auto depth = Texture2D(TextureFormat::D32_FLOAT_S8_UINT, 4, TextureUse::DepthStencilWrite);
    auto depthTarget = Texture2D(TextureFormat::D32_FLOAT_S8_UINT, 1, TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite);
    auto dsv = View(depth.get(), TextureViewUsage::DepthWrite);
    auto dstDsv = View(depthTarget.get(), TextureViewUsage::DepthWrite);
    auto readDsv = View(depthTarget.get(), TextureViewUsage::DepthRead);
    RenderPassDepthStencilAttachment ds{dsv.get(), LoadAction::Load, StoreAction::Store, LoadAction::Load, StoreAction::Store, {}, DepthStencilResolveDescriptor{dstDsv.get(), ResolveMode::None, ResolveMode::None}};
    pass = {.DepthStencil = ds, .Width = 8, .Height = 8};
    EXPECT_FALSE(ValidateRenderScope(pass, device));
    pass.DepthStencil->Resolve->DepthMode = static_cast<ResolveMode>(3);
    EXPECT_FALSE(ValidateRenderScope(pass, device));
    pass.DepthStencil->Resolve = {readDsv.get(), ResolveMode::SampleZero, ResolveMode::SampleZero};
    EXPECT_FALSE(ValidateRenderScope(pass, device));
}

TEST_P(AttachmentOperations, PartialPassClearAndDiscardPreserveOutsidePixelsAndLayers) {
    auto& device = *Context.Device;
    auto texture = device.CreateTexture({.Dim = TextureDimension::Dim2DArray, .Width = 8, .Height = 8, .DepthOrArraySize = 2, .MipLevels = 1, .SampleCount = 1, .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::RenderTarget | TextureUse::CopySource}).Unwrap();
    auto view = device.CreateTextureView({texture.get(), TextureDimension::Dim2DArray, TextureFormat::RGBA8_UNORM, {0, 2, 0, 1}, TextureViewUsage::RenderTarget}).Unwrap();
    const auto alignment = device.GetCapabilities().Detail.TextureDataPitchAlignment;
    const auto pitch = (32 + alignment - 1) / alignment * alignment;
    auto readback = device.CreateBuffer({.Size = pitch * 16, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor initial = BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&initial, 1});
    RenderPassColorAttachment color{view.get(), LoadAction::Clear, StoreAction::Store, {{0, 1, 0, 1}}};
    auto encoder = command->BeginRenderPass({.Colors = std::span{&color, 1}, .Width = 8, .Height = 8, .Layers = 2}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    Barrier(command.get(), texture.get(), TextureState::RenderTarget, TextureState::RenderTarget);
    color.Clear.Value = {1, 0, 0, 1};
    encoder = command->BeginRenderPass({.Colors = std::span{&color, 1}, .Width = 4, .Height = 4}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    Barrier(command.get(), texture.get(), TextureState::RenderTarget, TextureState::RenderTarget);
    color.Load = LoadAction::DontCare;
    color.Store = StoreAction::Discard;
    encoder = command->BeginRenderPass({.Colors = std::span{&color, 1}, .Width = 4, .Height = 4}).Unwrap();
    command->EndRenderPass(std::move(encoder));
    const ResourceBarrierDescriptor copy = BarrierTextureDescriptor{texture.get(), TextureState::RenderTarget, TextureState::CopySource};
    command->ResourceBarrier(std::span{&copy, 1});
    command->CopyTextureToBuffer(readback.get(), 0, texture.get(), {0, 2, 0, 1});
    test::GpuWorkGuard gpuWork{Context};
    Submit(command.get());
    const auto* pixels = static_cast<const uint8_t*>(readback->Map(0, pitch * 16));
    ASSERT_NE(pixels, nullptr);
    readback->InvalidateMappedRange({0, pitch * 16});
    for (uint32_t layer = 0; layer < 2; ++layer)
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x) {
                if (!layer && x < 4 && y < 4) continue;
                EXPECT_EQ(pixels[(layer * 8 + y) * pitch + x * 4 + 1], 255);
            }
    readback->Unmap();
    allocator->Reset();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_P(AttachmentOperations, MixedAttachmentsSampleTheReadOnlyAspectAtTheCurrentFragment) {
    auto& device = *Context.Device;
    for (const bool readDepth : {true, false}) {
        SCOPED_TRACE(readDepth);
        auto dsTexture = Texture2D(TextureFormat::D32_FLOAT_S8_UINT, 1, TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite | TextureUse::Resource);
        auto writeView = View(dsTexture.get(), TextureViewUsage::DepthWrite);
        auto mixedView = View(dsTexture.get(), readDepth ? TextureViewUsage::DepthReadStencilWrite : TextureViewUsage::DepthWriteStencilRead);
        const auto readAspect = readDepth ? TextureAspect::Depth : TextureAspect::Stencil;
        auto srv = View(dsTexture.get(), TextureViewUsage::Resource, readAspect);
        auto output = Texture2D(TextureFormat::R32_FLOAT, 1, TextureUse::RenderTarget | TextureUse::CopySource);
        auto rtv = View(output.get(), TextureViewUsage::RenderTarget);
        auto vs = Stage("attachment_sample_depth", ShaderStage::Vertex);
        auto ps = Stage(readDepth ? "attachment_sample_depth" : "attachment_sample_stencil", ShaderStage::Pixel);
        ASSERT_TRUE(vs && ps);
        const auto binding = Binding(0, BindingKind::Texture, 0, ShaderStage::Pixel);
        auto group = device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
        const PipelineLayoutGroupDescriptor slot{0, group.get()};
        auto layout = device.CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
        auto params = device.CreateShaderParameterSet({group.get()}).Unwrap();
        ASSERT_TRUE(params->Set(0, 0, srv.get()));
        ASSERT_TRUE(params->FlushWrites());
        auto primitive = PrimitiveState::Default();
        primitive.Cull = CullMode::None;
        auto depth = DepthStencilState::Default();
        depth.Format = TextureFormat::D32_FLOAT_S8_UINT;
        depth.DepthWriteEnable = !readDepth;
        depth.DepthCompare = CompareFunction::Always;
        depth.Stencil = StencilState::Default();
        if (readDepth) depth.Stencil->Front.PassOp = depth.Stencil->Back.PassOp = StencilOperation::IncrementClamp;
        const auto color = ColorTargetState::Default(TextureFormat::R32_FLOAT);
        GraphicsPipelineStateDescriptor pd{.PipelineLayout = layout.get(), .VS = ShaderEntry{vs.get(), "VSMain"}, .PS = ShaderEntry{ps.get(), "PSMain"}, .Primitive = primitive, .DepthStencil = depth, .MultiSample = {1, ~uint64_t{0}, false}, .ColorTargets = std::span{&color, 1}};
        auto pipeline = device.CreateGraphicsPipelineState(pd).Unwrap();
        pd.DepthStencil->DepthWriteEnable = true;
        pd.DepthStencil->Stencil->Front.PassOp = pd.DepthStencil->Stencil->Back.PassOp = StencilOperation::IncrementClamp;
        auto invalidPipeline = device.CreateGraphicsPipelineState(pd).Unwrap();
        const auto alignment = device.GetCapabilities().Detail.TextureDataPitchAlignment;
        const auto pitch = (32 + alignment - 1) / alignment * alignment;
        auto readback = device.CreateBuffer({.Size = pitch * 8, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
        auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
        auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
        command->Begin();
        Barrier(command.get(), dsTexture.get(), TextureState::Undefined, TextureState::DepthWrite);
        Barrier(command.get(), output.get(), TextureState::Undefined, TextureState::RenderTarget);
        RenderPassDepthStencilAttachment ds{writeView.get(), LoadAction::Clear, StoreAction::Store, LoadAction::Clear, StoreAction::Store, {0.75f, 9}};
        auto encoder = command->BeginRenderPass({.DepthStencil = ds, .Width = 8, .Height = 8}).Unwrap();
        command->EndRenderPass(std::move(encoder));
        const auto mixed = readDepth ? TextureState::DepthReadStencilWrite : TextureState::DepthWriteStencilRead;
        if (device.GetCapabilities().Features.IndependentDepthStencilTransitions) {
            Barrier(command.get(), dsTexture.get(), TextureState::DepthWrite, TextureState::DepthRead | TextureState::ShaderRead, readAspect);
            // Order the previous store of the other aspect without changing its state.
            Barrier(command.get(), dsTexture.get(), TextureState::DepthWrite, TextureState::DepthWrite, readDepth ? TextureAspect::Stencil : TextureAspect::Depth);
        } else
            Barrier(command.get(), dsTexture.get(), TextureState::DepthWrite, mixed | TextureState::ShaderRead);
        ds.View = mixedView.get();
        ds.DepthLoad = ds.StencilLoad = LoadAction::Load;
        const RenderPassColorAttachment attachment{rtv.get(), LoadAction::Clear, StoreAction::Store, {}};
        encoder = command->BeginRenderPass({.Colors = std::span{&attachment, 1}, .DepthStencil = ds, .Width = 8, .Height = 8}).Unwrap();
        const auto errors = Context.RhiValidationErrors.load();
        encoder->BindGraphicsPipelineState(invalidPipeline.get());
        EXPECT_EQ(Context.RhiValidationErrors.load(), errors + 1);
        encoder->BindGraphicsPipelineState(pipeline.get());
        encoder->BindShaderParameterSet(0, params.get(), {});
        encoder->SetViewport({0, 0, 8, 8, 0, 1});
        encoder->SetScissor({0, 0, 8, 8});
        encoder->Draw(3, 1, 0, 0);
        command->EndRenderPass(std::move(encoder));
        Barrier(command.get(), output.get(), TextureState::RenderTarget, TextureState::CopySource);
        command->CopyTextureToBuffer(readback.get(), 0, output.get(), {0, 1, 0, 1});
        test::GpuWorkGuard gpuWork{Context};
        Submit(command.get());
        auto* pixels = static_cast<const byte*>(readback->Map(0, pitch * 8));
        ASSERT_NE(pixels, nullptr);
        readback->InvalidateMappedRange({0, pitch * 8});
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x)
                EXPECT_FLOAT_EQ(reinterpret_cast<const float*>(pixels + y * pitch)[x], readDepth ? 0.75f : 9.0f);
        readback->Unmap();
        allocator->Reset();
    }
}

TEST_P(AttachmentOperations, OrdinaryCachedAndImmutableSamplersUseTheSameValidation) {
    auto& device = *Context.Device;
    const auto& caps = device.GetCapabilities();
    const auto check = [&](const SamplerDescriptor& desc, bool supported) {
        auto ordinary = device.CreateSampler(desc);
        auto cached = device.GetOrCreateSampler(desc);
        auto binding = Binding(1, BindingKind::Sampler, 0);
        binding.ImmutableSampler = desc;
        auto group = device.CreateBindingGroupLayout({std::span{&binding, 1}});
        EXPECT_EQ(bool(ordinary), supported);
        EXPECT_EQ(bool(cached), supported);
        EXPECT_EQ(bool(group), supported);
        if (cached) EXPECT_EQ(cached.Get(), device.GetOrCreateSampler(desc).Get());
    };
    SamplerDescriptor desc{};
    check(desc, true);
    desc.AddressS = AddressMode::MirrorClampToEdge;
    check(desc, caps.Features.SamplerMirrorClampToEdge);
    desc = {};
    desc.AnisotropyClamp = 4;
    check(desc, caps.Features.SamplerAnisotropyPointMip && caps.Limits.MaxSamplerAnisotropy >= 4);
    desc.MipmapFilter = FilterMode::Linear;
    check(desc, caps.Features.SamplerAnisotropy && caps.Limits.MaxSamplerAnisotropy >= 4);
    desc = {};
    desc.Reduction = SamplerReductionMode::Min;
    check(desc, caps.Features.SamplerFilterMinmax);
    desc.Compare = CompareFunction::Less;
    check(desc, false);
    desc = {};
    desc.AddressS = desc.AddressT = AddressMode::ClampToEdge;
    desc.UnnormalizedCoordinates = true;
    check(desc, caps.Features.SamplerUnnormalizedCoordinates);
    desc.AnisotropyClamp = 4;
    check(desc, false);
    desc = {};
    desc.MipLodBias = caps.Limits.MaxSamplerLodBias + 1;
    check(desc, false);
    desc = {};
    desc.BorderColor = SamplerBorderColor::IntOpaqueWhite;
    check(desc, caps.Features.SamplerIntegerBorderColor);
}

TEST_P(AttachmentOperations, SamplerReductionReadbackMatchesForOrdinaryAndImmutableBindings) {
    auto& device = *Context.Device;
    const auto support = device.QueryTextureSupport({TextureDimension::Dim2D, TextureFormat::R32_FLOAT, TextureUse::Resource});
    if (!support.MinMaxFiltering) GTEST_SKIP() << "R32_FLOAT min/max filtering unavailable";
    EXPECT_TRUE(device.GetCapabilities().Features.SamplerFilterMinmax);
    const auto alignment = device.GetCapabilities().Detail.TextureDataPitchAlignment;
    const auto pitch = (32 + alignment - 1) / alignment * alignment;
    vector<float> pixels(pitch * 8 / sizeof(float), 0);
    pixels[3 * pitch / 4 + 3] = 1;
    pixels[3 * pitch / 4 + 4] = 2;
    pixels[4 * pitch / 4 + 3] = 3;
    pixels[4 * pitch / 4 + 4] = 4;
    auto upload = test::MakeUploadBuffer(device, std::as_bytes(std::span{pixels}), BufferUse::CopySource).Unwrap();
    auto texture = Texture2D(TextureFormat::R32_FLOAT, 1, TextureUse::Resource | TextureUse::CopyDestination);
    auto view = View(texture.get(), TextureViewUsage::Resource);
    auto shader = Stage("sampler_reduction", ShaderStage::Compute);
    ASSERT_TRUE(shader);
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor uploadReady = BarrierBufferDescriptor{upload.get(), BufferState::HostWrite, BufferState::CopySource};
    command->ResourceBarrier(std::span{&uploadReady, 1});
    Barrier(command.get(), texture.get(), TextureState::Undefined, TextureState::CopyDestination);
    command->CopyBufferToTexture(texture.get(), {0, 1, 0, 1}, upload.get(), 0);
    Barrier(command.get(), texture.get(), TextureState::CopyDestination, TextureState::ShaderRead);
    test::GpuWorkGuard gpuWork{Context};
    Submit(command.get());
    allocator->Reset();
    for (const bool immutable : {false, true})
        for (const auto reduction : {SamplerReductionMode::WeightedAverage, SamplerReductionMode::Min, SamplerReductionMode::Max}) {
            SCOPED_TRACE(fmt::format("immutable={} reduction={}", immutable, static_cast<uint32_t>(reduction)));
            SamplerDescriptor desc{};
            desc.AddressS = desc.AddressT = AddressMode::ClampToEdge;
            desc.MinFilter = desc.MagFilter = FilterMode::Linear;
            desc.Reduction = reduction;
            auto sampler = device.CreateSampler(desc).Unwrap();
            BindingDescriptor bindings[]{Binding(0, BindingKind::Texture, 0), Binding(1, BindingKind::Sampler, 0), Binding(2, BindingKind::RWStructuredBuffer, 0)};
            if (immutable) bindings[1].ImmutableSampler = desc;
            auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
            const PipelineLayoutGroupDescriptor slot{0, group.get()};
            auto layout = device.CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
            auto pipeline = device.CreateComputePipelineState({layout.get(), {shader.get(), "CSMain"}}).Unwrap();
            auto output = device.CreateBuffer({.Size = 4, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
            auto readback = device.CreateBuffer({.Size = 4, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
            auto params = device.CreateShaderParameterSet({group.get()}).Unwrap();
            ASSERT_TRUE(params->Set(0, 0, view.get()));
            if (!immutable) ASSERT_TRUE(params->Set(1, 0, sampler.get()));
            ASSERT_TRUE(params->Set(2, 0, ShaderBufferBinding{output.get(), {0, 4}, 4}));
            ASSERT_TRUE(params->FlushWrites());
            command->Begin();
            const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{output.get(), BufferState::Undefined, BufferState::UnorderedAccess};
            command->ResourceBarrier(std::span{&toUav, 1});
            auto encoder = command->BeginComputePass().Unwrap();
            encoder->BindComputePipelineState(pipeline.get());
            encoder->BindShaderParameterSet(0, params.get(), {});
            encoder->Dispatch(1, 1, 1);
            command->EndComputePass(std::move(encoder));
            const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{output.get(), BufferState::UnorderedAccess, BufferState::CopySource};
            command->ResourceBarrier(std::span{&toCopy, 1});
            command->CopyBufferToBuffer(readback.get(), 0, output.get(), 0, 4);
            Submit(command.get());
            auto* value = static_cast<const float*>(readback->Map(0, 4));
            ASSERT_NE(value, nullptr);
            readback->InvalidateMappedRange({0, 4});
            EXPECT_NEAR(*value, reduction == SamplerReductionMode::Min ? 1.0f : reduction == SamplerReductionMode::Max ? 4.0f
                                                                                                                       : 2.5f,
                        0.0001f);
            readback->Unmap();
            allocator->Reset();
        }
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_P(AttachmentOperations, MinMaxFilteringIsReportedPerFormat) {
    auto& device = *Context.Device;
    if (!device.GetCapabilities().Features.SamplerFilterMinmax) GTEST_SKIP() << "device min/max reduction unavailable";
    for (int32_t value = 1; value <= static_cast<int32_t>(TextureFormat::D32_FLOAT_S8_UINT); ++value) {
        const auto support = device.QueryTextureSupport({TextureDimension::Dim2D, static_cast<TextureFormat>(value), TextureUse::Resource});
        if (support.Supported && !support.MinMaxFiltering) return;
    }
    GTEST_SKIP() << "adapter supports min/max filtering for every supported RHI sampling format";
}

INSTANTIATE_TEST_SUITE_P(BackendsAndFeatures, AttachmentOperations, testing::Values(0, 1, 2, 3, 4));

}  // namespace
}  // namespace radray::render
