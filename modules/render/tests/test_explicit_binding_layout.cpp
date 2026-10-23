#include "gpu_test_fixture.h"
#include <filesystem>
#include <fstream>
#include <radray/scope_guard.h>
#ifdef RADRAY_ENABLE_D3D12
#include <dxgimessages.h>
#endif

namespace radray::render {
namespace {

BindingDescriptor MakeBinding(uint32_t id, BindingKind kind, uint32_t dx, uint32_t vk, uint32_t set = 0, bool dynamic = false) {
    return {.BindingId = id, .Kind = kind, .Count = 1, .Stages = ShaderStage::Compute, .D3D12 = D3D12BindingDescriptor{GetBindingRegisterClass(kind), dx, 0, dynamic ? D3D12BufferPlacement::RootDescriptor : D3D12BufferPlacement::Table}, .Vulkan = VulkanBindingDescriptor{set, vk, dynamic ? VulkanBufferDescriptorPlacement::Dynamic : VulkanBufferDescriptorPlacement::Regular}};
}

vector<byte> ReadStage(std::string_view name, RenderBackend backend) {
    const auto path = std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/shader_bytecode" /
                      (string{name} + (backend == RenderBackend::D3D12 ? ".CSMain.dxil.bin" : ".CSMain.spirv.bin"));
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() <= 0) return {};
    vector<byte> bytes(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file ? bytes : vector<byte>{};
}

class ExplicitBindingLayoutTest : public testing::TestWithParam<RenderBackend> {
protected:
    test::DeviceContext Context;
    void SetUp() override {
        if (!test::TryCreateDevice(GetParam(), Context, true, true)) {
            if (test::SetupMustFail(Context.Status, test::RequiredBackend(GetParam()))) FAIL() << Context.Reason;
            GTEST_SKIP() << Context.Reason;
        }
    }
    void TearDown() override {
        Context.Reset();
    }
};

TEST_P(ExplicitBindingLayoutTest, SharesGroupAndParametersAcrossPipelinesWithDifferentRootIndices) {
    auto& device = *Context.Device;
    const auto bytes = ReadStage("compute", GetParam());
    ASSERT_FALSE(bytes.empty());
    auto shader = device.CreateShader({bytes, GetParam() == RenderBackend::D3D12 ? ShaderBlobCategory::DXIL : ShaderBlobCategory::SPIRV, ShaderStage::Compute}).Unwrap();
    for (bool dynamic : {false, true}) {
        SCOPED_TRACE(dynamic);
        const auto binding = MakeBinding(91, BindingKind::RWStructuredBuffer, 2, 6, 0, dynamic);
        auto unusedSampler = MakeBinding(92, BindingKind::Sampler, 9, 9);
        unusedSampler.ImmutableSampler = SamplerDescriptor{};
        const BindingDescriptor descriptors[]{binding, unusedSampler};
        auto group = device.CreateBindingGroupLayout({descriptors}).Unwrap();
        EXPECT_EQ(group->GetDesc().Bindings.size(), 2u);
        auto separateGroup = device.CreateBindingGroupLayout({descriptors}).Unwrap();
        const PipelineLayoutGroupDescriptor separateSlot{7, separateGroup.get()};
        auto separateLayout = device.CreatePipelineLayout({std::span{&separateSlot, 1}, {}}).Unwrap();
        const PipelineLayoutGroupDescriptor slot{7, group.get()};
        auto layout = device.CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
        const PushConstantDescriptor unusedPush{77, 4, ShaderStage::Compute, PushConstantDescriptor::D3D12Location{9, 0}, PushConstantDescriptor::VulkanLocation{0}};
        auto otherLayout = device.CreatePipelineLayout({std::span{&slot, 1}, std::span{&unusedPush, 1}}).Unwrap();
        auto output = device.CreateBuffer({.Size = 1024, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
        auto readback = device.CreateBuffer({.Size = 4, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
        auto parameters = device.CreateShaderParameterSet({group.get()}).Unwrap();
        ASSERT_TRUE(parameters->Set(91, 0, ShaderBufferBinding{output.get(), {0, 4}, 4}));
        ASSERT_TRUE(parameters->FlushWrites());
        BufferState before = BufferState::Undefined;
        uint32_t dynamicOffset = 0;
        for (auto* target : {layout.get(), layout.get(), otherLayout.get(), separateLayout.get()}) {
            auto pso = device.CreateComputePipelineState({target, {shader.get(), "CSMain"}}).Unwrap();
            auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
            auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
            command->Begin();
            const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{output.get(), before, BufferState::UnorderedAccess};
            command->ResourceBarrier(std::span{&toUav, 1});
            auto encoder = command->BeginComputePass().Unwrap();
            encoder->BindComputePipelineState(pso.get());
            const ShaderParameterDynamicOffset offset{91, 0, dynamicOffset};
            encoder->BindShaderParameterSet(7, parameters.get(), dynamic ? std::span{&offset, 1} : std::span<const ShaderParameterDynamicOffset>{});
            encoder->Dispatch(1, 1, 1);
            command->EndComputePass(std::move(encoder));
            const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{output.get(), BufferState::UnorderedAccess, BufferState::CopySource};
            command->ResourceBarrier(std::span{&toCopy, 1});
            command->CopyBufferToBuffer(readback.get(), 0, output.get(), dynamic ? dynamicOffset : 0, 4);
            command->End();
            CommandBuffer* commands[]{command.get()};
            test::GpuWorkGuard gpuWork{Context};
            Context.Queue->Submit({.CmdBuffers = commands});
            Context.Queue->Wait();
            auto* mapped = static_cast<uint32_t*>(readback->Map(0, 4));
            ASSERT_NE(mapped, nullptr);
            readback->InvalidateMappedRange({0, 4});
            EXPECT_EQ(*mapped, 0x12345678u);
            readback->Unmap();
            before = BufferState::CopySource;
            dynamicOffset = 256;
        }
        otherLayout->Destroy();
        otherLayout->Destroy();
        EXPECT_TRUE(group->IsValid());
        parameters->ResetBindingsForReuse();
        EXPECT_TRUE(parameters->Set(91, 0, ShaderBufferBinding{output.get(), {0, 4}, 4}));
    }
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_P(ExplicitBindingLayoutTest, RetainsViewAndMaterialWhenOnlyObjectGroupIsRebound) {
    auto& device = *Context.Device;
    const auto bytes = ReadStage("explicit_layered_groups", GetParam());
    ASSERT_FALSE(bytes.empty());
    auto shader = device.CreateShader({bytes, GetParam() == RenderBackend::D3D12 ? ShaderBlobCategory::DXIL : ShaderBlobCategory::SPIRV, ShaderStage::Compute}).Unwrap();
    constexpr uint32_t viewCount = 2, materialCount = 2, objectsPerMaterial = 3;
    constexpr uint32_t dispatchCount = viewCount * materialCount * objectsPerMaterial;
    const uint32_t stride = static_cast<uint32_t>(std::max(uint64_t{256}, device.GetCapabilities().Limits.StorageBufferOffsetAlignment));
    const uint32_t wordsPerRecord = stride / sizeof(uint32_t);
    const uint32_t objectStart = viewCount + materialCount;
    vector<uint32_t> inputValues((objectStart + dispatchCount) * wordsPerRecord, 0);
    inputValues[0] = 101;
    inputValues[wordsPerRecord] = 202;
    inputValues[viewCount * wordsPerRecord] = 11;
    inputValues[(viewCount + 1) * wordsPerRecord] = 22;
    for (uint32_t i = 0; i < dispatchCount; ++i) inputValues[(objectStart + i) * wordsPerRecord] = i + 1;

    for (bool dynamic : {false, true}) {
        SCOPED_TRACE(dynamic);
        // The same BindingId belongs to three independent group namespaces.
        const auto viewBinding = MakeBinding(7, BindingKind::StructuredBuffer, 1, 3, 0);
        const auto materialBinding = MakeBinding(7, BindingKind::StructuredBuffer, 4, 5, 1);
        const BindingDescriptor objectBindings[]{
            MakeBinding(7, BindingKind::StructuredBuffer, 8, 2, 2, dynamic),
            MakeBinding(9, BindingKind::RWStructuredBuffer, 2, 7, 2, dynamic)};
        auto viewLayout = device.CreateBindingGroupLayout({std::span{&viewBinding, 1}}).Unwrap();
        auto materialLayout = device.CreateBindingGroupLayout({std::span{&materialBinding, 1}}).Unwrap();
        auto objectLayout = device.CreateBindingGroupLayout({objectBindings}).Unwrap();
        const PipelineLayoutGroupDescriptor groups[]{
            {4, viewLayout.get()}, {8, materialLayout.get()}, {12, objectLayout.get()}};
        auto layout = device.CreatePipelineLayout({groups, {}}).Unwrap();
        auto pso = device.CreateComputePipelineState({layout.get(), {shader.get(), "CSMain"}}).Unwrap();
        auto input = test::MakeUploadBuffer(device, std::as_bytes(std::span{inputValues}), BufferUse::Resource).Unwrap();
        auto output = device.CreateBuffer({.Size = dispatchCount * stride, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
        auto readback = device.CreateBuffer({.Size = dispatchCount * stride, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
        vector<unique_ptr<ShaderParameterSet>> viewSets, materialSets, objectSets;
        for (uint32_t i = 0; i < viewCount; ++i) {
            auto set = device.CreateShaderParameterSet({viewLayout.get()}).Unwrap();
            ASSERT_TRUE(set->Set(7, 0, ShaderBufferBinding{input.get(), {i * stride, 4}, 4}));
            ASSERT_TRUE(set->FlushWrites());
            viewSets.push_back(std::move(set));
        }
        for (uint32_t i = 0; i < materialCount; ++i) {
            auto set = device.CreateShaderParameterSet({materialLayout.get()}).Unwrap();
            ASSERT_TRUE(set->Set(7, 0, ShaderBufferBinding{input.get(), {(viewCount + i) * stride, 4}, 4}));
            ASSERT_TRUE(set->FlushWrites());
            materialSets.push_back(std::move(set));
        }
        for (uint32_t i = 0; i < (dynamic ? 1u : dispatchCount); ++i) {
            auto set = device.CreateShaderParameterSet({objectLayout.get()}).Unwrap();
            ASSERT_TRUE(set->Set(7, 0, ShaderBufferBinding{input.get(), {(objectStart + i) * stride, 4}, 4}));
            ASSERT_TRUE(set->Set(9, 0, ShaderBufferBinding{output.get(), {i * stride, 4}, 4}));
            ASSERT_TRUE(set->FlushWrites());
            objectSets.push_back(std::move(set));
        }

        auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
        auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
        command->Begin();
        const ResourceBarrierDescriptor initial[]{
            BarrierBufferDescriptor{input.get(), BufferState::HostWrite, BufferState::ShaderRead},
            BarrierBufferDescriptor{output.get(), BufferState::Undefined, BufferState::UnorderedAccess}};
        command->ResourceBarrier(initial);
        auto encoder = command->BeginComputePass().Unwrap();
        encoder->BindComputePipelineState(pso.get());
        vector<uint32_t> expected;
        for (uint32_t view = 0; view < viewCount; ++view) {
            encoder->BindShaderParameterSet(4, viewSets[view].get());
            for (uint32_t material = 0; material < materialCount; ++material) {
                encoder->BindShaderParameterSet(8, materialSets[material].get());
                for (uint32_t object = 0; object < objectsPerMaterial; ++object) {
                    const uint32_t index = static_cast<uint32_t>(expected.size());
                    const ShaderParameterDynamicOffset offsets[]{{9, 0, index * stride}, {7, 0, index * stride}};
                    encoder->BindShaderParameterSet(12, objectSets[dynamic ? 0 : index].get(), dynamic ? std::span{offsets} : std::span<const ShaderParameterDynamicOffset>{});
                    // Each dispatch writes a disjoint four-byte output slice.
                    encoder->Dispatch(1, 1, 1);
                    expected.push_back(inputValues[view * wordsPerRecord] * 10000 + inputValues[(viewCount + material) * wordsPerRecord] * 100 + index + 1);
                }
            }
        }
        command->EndComputePass(std::move(encoder));
        const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{output.get(), BufferState::UnorderedAccess, BufferState::CopySource};
        command->ResourceBarrier(std::span{&toCopy, 1});
        command->CopyBufferToBuffer(readback.get(), 0, output.get(), 0, dispatchCount * stride);
        command->End();
        CommandBuffer* commands[]{command.get()};
        test::GpuWorkGuard gpuWork{Context};
        Context.Queue->Submit({.CmdBuffers = commands});
        Context.Queue->Wait();
        auto* mapped = static_cast<uint32_t*>(readback->Map(0, dispatchCount * stride));
        ASSERT_NE(mapped, nullptr);
        readback->InvalidateMappedRange({0, dispatchCount * stride});
        for (uint32_t i = 0; i < dispatchCount; ++i) EXPECT_EQ(mapped[i * wordsPerRecord], expected[i]) << "dispatch " << i;
        readback->Unmap();
        EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
    }
}

TEST_P(ExplicitBindingLayoutTest, IndependentD3D12PushBlocksAndSingleVulkanBlock) {
    auto& device = *Context.Device;
    const auto bytes = ReadStage("explicit_multiple_push", GetParam());
    ASSERT_FALSE(bytes.empty());
    auto shader = device.CreateShader({bytes, GetParam() == RenderBackend::D3D12 ? ShaderBlobCategory::DXIL : ShaderBlobCategory::SPIRV, ShaderStage::Compute}).Unwrap();
    const auto binding = MakeBinding(1, BindingKind::RWStructuredBuffer, 2, 6);
    auto group = device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    const PipelineLayoutGroupDescriptor slot{2, group.get()};
    vector<PushConstantDescriptor> pushes{
        {100, 4, ShaderStage::Compute, PushConstantDescriptor::D3D12Location{9, 0}, PushConstantDescriptor::VulkanLocation{0}},
        {200, 4, ShaderStage::Compute, PushConstantDescriptor::D3D12Location{10, 0}, std::nullopt}};
    if (GetParam() == RenderBackend::Vulkan) {
        pushes.resize(1);
        pushes[0].Size = 8;
    }
    auto layout = device.CreatePipelineLayout({std::span{&slot, 1}, pushes}).Unwrap();
    auto pso = device.CreateComputePipelineState({layout.get(), {shader.get(), "CSMain"}}).Unwrap();
    auto output = device.CreateBuffer({.Size = 4, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
    auto readback = device.CreateBuffer({.Size = 4, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    auto set = device.CreateShaderParameterSet({group.get()}).Unwrap();
    ASSERT_TRUE(set->Set(1, 0, ShaderBufferBinding{output.get(), {0, 4}, 4}));
    ASSERT_TRUE(set->FlushWrites());
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{output.get(), BufferState::Undefined, BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&toUav, 1});
    auto encoder = command->BeginComputePass().Unwrap();
    encoder->BindComputePipelineState(pso.get());
    encoder->BindShaderParameterSet(2, set.get());
    const uint32_t first = 23, second = 19;
    ASSERT_TRUE(encoder->SetPushConstants(100, 0, std::as_bytes(std::span{&first, 1})));
    ASSERT_TRUE(encoder->SetPushConstants(GetParam() == RenderBackend::D3D12 ? 200 : 100,
                                          GetParam() == RenderBackend::D3D12 ? 0 : 4, std::as_bytes(std::span{&second, 1})));
    encoder->Dispatch(1, 1, 1);
    command->EndComputePass(std::move(encoder));
    const ResourceBarrierDescriptor copy = BarrierBufferDescriptor{output.get(), BufferState::UnorderedAccess, BufferState::CopySource};
    command->ResourceBarrier(std::span{&copy, 1});
    command->CopyBufferToBuffer(readback.get(), 0, output.get(), 0, 4);
    command->End();
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard gpuWork{Context};
    Context.Queue->Submit({.CmdBuffers = commands});
    Context.Queue->Wait();
    auto* mapped = static_cast<uint32_t*>(readback->Map(0, 4));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, 4});
    EXPECT_EQ(*mapped, 42u);
    readback->Unmap();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 0u);
}

TEST_P(ExplicitBindingLayoutTest, ValidationRejectsLocalMisuseAndIgnoresInactiveBackendMappings) {
    auto& device = *Context.Device;
    auto binding = MakeBinding(8, BindingKind::CBuffer, 3, 2, 3, true);
    if (GetParam() == RenderBackend::D3D12)
        binding.Vulkan.reset();
    else
        binding.D3D12.reset();
    auto group = device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    auto set = device.CreateShaderParameterSet({group.get()}).Unwrap();
    auto buffer = device.CreateBuffer({.Size = 512, .Memory = MemoryType::Upload, .Usage = BufferUse::CBuffer | BufferUse::MapWrite}).Unwrap();
    EXPECT_FALSE(set->Set(9, 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    EXPECT_FALSE(set->Set(8, 1, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    EXPECT_FALSE(set->Set(8, 0, ShaderBufferBinding{buffer.get(), {512, 256}, 0}));
    EXPECT_TRUE(set->Set(8, 0, ShaderBufferBinding{buffer.get(), {0, 256}, 0}));
    EXPECT_EQ(Context.RhiValidationErrors.load(), 3u);
    binding.Count = 2;
    EXPECT_FALSE(device.CreateBindingGroupLayout({std::span{&binding, 1}}));
}

TEST_P(ExplicitBindingLayoutTest, SparseNativeSetArraysSamplersBuffersAndPartialPushUpdates) {
    auto& device = *Context.Device;
    const auto bytes = ReadStage("explicit_bindings", GetParam());
    ASSERT_FALSE(bytes.empty());
    auto shader = device.CreateShader({bytes, GetParam() == RenderBackend::D3D12 ? ShaderBlobCategory::DXIL : ShaderBlobCategory::SPIRV, ShaderStage::Compute}).Unwrap();
    vector<BindingDescriptor> bindings{
        MakeBinding(80, BindingKind::RWStructuredBuffer, 2, 6, 3, true),
        MakeBinding(10, BindingKind::StructuredBuffer, 4, 8, 3, true),
        MakeBinding(20, BindingKind::RawBuffer, 6, 1, 3),
        MakeBinding(21, BindingKind::TypedBuffer, 7, 2, 3),
        MakeBinding(30, BindingKind::CBuffer, 1, 4, 3),
        MakeBinding(40, BindingKind::StructuredBuffer, 8, 10, 3),
        MakeBinding(50, BindingKind::RWRawBuffer, 4, 12, 3),
        MakeBinding(51, BindingKind::RWTypedBuffer, 5, 13, 3),
        MakeBinding(60, BindingKind::Texture, 10, 14, 3),
        MakeBinding(61, BindingKind::RWTexture, 6, 15, 3),
        MakeBinding(70, BindingKind::Sampler, 0, 16, 3),
        MakeBinding(71, BindingKind::Sampler, 1, 17, 3)};
    bindings[5].Count = 2;
    SamplerDescriptor samplerDesc{};
    samplerDesc.MinFilter = samplerDesc.MagFilter = FilterMode::Linear;
    bindings.back().ImmutableSampler = samplerDesc;
    auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
    bindings.clear();  // Creation copied all descriptor storage.
    const PipelineLayoutGroupDescriptor slot{11, group.get()};
    const PushConstantDescriptor push{77, 8, ShaderStage::Compute, PushConstantDescriptor::D3D12Location{9, 0}, PushConstantDescriptor::VulkanLocation{0}};
    auto layout = device.CreatePipelineLayout({std::span{&slot, 1}, std::span{&push, 1}}).Unwrap();
    auto pso = device.CreateComputePipelineState({layout.get(), {shader.get(), "CSMain"}}).Unwrap();
    vector<uint32_t> values(128, 0);
    values[0] = 5;
    values[64] = 17;
    auto input = test::MakeUploadBuffer(device, std::as_bytes(std::span{values}), BufferUse::Resource | BufferUse::CBuffer).Unwrap();
    auto output = device.CreateBuffer({.Size = 1024, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
    auto rawOutput = device.CreateBuffer({.Size = 256, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
    auto typedOutput = device.CreateBuffer({.Size = 256, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
    auto readback = device.CreateBuffer({.Size = 1024, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
    vector<float> pixels(64, 0);
    pixels[0] = pixels[3] = 1;
    auto upload = test::MakeUploadBuffer(device, std::as_bytes(std::span{pixels}), BufferUse::CopySource).Unwrap();
    const TextureDescriptor textureDesc{TextureDimension::Dim2D, 1, 1, 1, 1, 1, TextureFormat::RGBA32_FLOAT, MemoryType::Device,
                                        TextureUse::Resource | TextureUse::CopyDestination};
    auto texture = device.CreateTexture(textureDesc).Unwrap();
    auto imageDesc = textureDesc;
    imageDesc.Usage = TextureUse::UnorderedAccess | TextureUse::CopySource;
    auto image = device.CreateTexture(imageDesc).Unwrap();
    auto textureView = device.CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA32_FLOAT, {0, 1, 0, 1}, TextureViewUsage::Resource}).Unwrap();
    auto imageView = device.CreateTextureView({image.get(), TextureDimension::Dim2D, TextureFormat::RGBA32_FLOAT, {0, 1, 0, 1}, TextureViewUsage::UnorderedAccess}).Unwrap();
    auto sampler = device.CreateSampler(samplerDesc).Unwrap();
    auto parameters = device.CreateShaderParameterSet({group.get()}).Unwrap();
    ASSERT_TRUE(parameters->Set(80, 0, ShaderBufferBinding{output.get(), {0, 4}, 4}));
    ASSERT_TRUE(parameters->Set(10, 0, ShaderBufferBinding{input.get(), {0, 4}, 4}));
    ASSERT_TRUE(parameters->Set(20, 0, ShaderBufferBinding{input.get(), {0, 4}, 0}));
    ASSERT_TRUE(parameters->Set(21, 0, ShaderTexelBufferBinding{input.get(), {0, 4}, TextureFormat::R32_UINT}));
    ASSERT_TRUE(parameters->Set(30, 0, ShaderBufferBinding{input.get(), {0, 256}, 0}));
    for (uint32_t i = 0; i < 2; ++i) ASSERT_TRUE(parameters->Set(40, i, ShaderBufferBinding{input.get(), {0, 4}, 4}));
    ASSERT_TRUE(parameters->Set(50, 0, ShaderBufferBinding{rawOutput.get(), {0, 4}, 0}));
    ASSERT_TRUE(parameters->Set(51, 0, ShaderTexelBufferBinding{typedOutput.get(), {0, 4}, TextureFormat::R32_UINT}));
    ASSERT_TRUE(parameters->Set(60, 0, textureView.get()));
    ASSERT_TRUE(parameters->Set(61, 0, imageView.get()));
    ASSERT_TRUE(parameters->Set(70, 0, sampler.get()));
    EXPECT_FALSE(parameters->Set(71, 0, sampler.get()));
    ASSERT_TRUE(parameters->FlushWrites());
    auto allocator = device.CreateCommandAllocator(Context.Queue).Unwrap();
    auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
    command->Begin();
    const ResourceBarrierDescriptor initial[]{
        BarrierBufferDescriptor{input.get(), BufferState::HostWrite, BufferState::ShaderRead | BufferState::CBuffer},
        BarrierBufferDescriptor{upload.get(), BufferState::HostWrite, BufferState::CopySource},
        BarrierBufferDescriptor{output.get(), BufferState::Undefined, BufferState::UnorderedAccess},
        BarrierBufferDescriptor{rawOutput.get(), BufferState::Undefined, BufferState::UnorderedAccess},
        BarrierBufferDescriptor{typedOutput.get(), BufferState::Undefined, BufferState::UnorderedAccess},
        BarrierTextureDescriptor{texture.get(), TextureState::Undefined, TextureState::CopyDestination},
        BarrierTextureDescriptor{image.get(), TextureState::Undefined, TextureState::UnorderedAccess}};
    command->ResourceBarrier(initial);
    command->CopyBufferToTexture(texture.get(), {0, 1, 0, 1}, upload.get(), 0);
    const ResourceBarrierDescriptor ready = BarrierTextureDescriptor{texture.get(), TextureState::CopyDestination, TextureState::ShaderRead};
    command->ResourceBarrier(std::span{&ready, 1});
    auto encoder = command->BeginComputePass().Unwrap();
    encoder->BindComputePipelineState(pso.get());
    // Deliberately reverse native Vulkan binding order. IDs, not span order, choose offsets.
    const ShaderParameterDynamicOffset offsets[]{{10, 0, 256}, {80, 0, 256}};
    encoder->BindShaderParameterSet(11, parameters.get(), std::span{offsets, 1});
    const ShaderParameterDynamicOffset duplicate[]{{10, 0, 0}, {10, 0, 0}};
    encoder->BindShaderParameterSet(11, parameters.get(), duplicate);
    const ShaderParameterDynamicOffset overflow[]{{10, 0, 512}, {80, 0, 256}};
    encoder->BindShaderParameterSet(11, parameters.get(), overflow);
    encoder->BindShaderParameterSet(11, parameters.get(), offsets);
    EXPECT_FALSE(parameters->Set(80, 0, ShaderBufferBinding{output.get(), {0, 4}, 4}));
    const uint32_t first = 7, second = 11;
    ASSERT_TRUE(encoder->SetPushConstants(77, 0, std::as_bytes(std::span{&first, 1})));
    ASSERT_TRUE(encoder->SetPushConstants(77, 4, std::as_bytes(std::span{&second, 1})));
    EXPECT_FALSE(encoder->SetPushConstants(77, 8, std::as_bytes(std::span{&second, 1})));
    EXPECT_FALSE(encoder->SetPushConstants(77, 2, std::as_bytes(std::span{&second, 1})));
    encoder->Dispatch(1, 1, 1);
    command->EndComputePass(std::move(encoder));
    const ResourceBarrierDescriptor copies[]{
        BarrierBufferDescriptor{output.get(), BufferState::UnorderedAccess, BufferState::CopySource},
        BarrierBufferDescriptor{rawOutput.get(), BufferState::UnorderedAccess, BufferState::CopySource},
        BarrierBufferDescriptor{typedOutput.get(), BufferState::UnorderedAccess, BufferState::CopySource},
        BarrierTextureDescriptor{image.get(), TextureState::UnorderedAccess, TextureState::CopySource}};
    command->ResourceBarrier(copies);
    command->CopyBufferToBuffer(readback.get(), 0, output.get(), 256, 4);
    command->CopyBufferToBuffer(readback.get(), 4, rawOutput.get(), 0, 4);
    command->CopyBufferToBuffer(readback.get(), 8, typedOutput.get(), 0, 4);
    command->CopyTextureToBuffer(readback.get(), 512, image.get(), {0, 1, 0, 1});
    command->End();
    CommandBuffer* commands[]{command.get()};
    test::GpuWorkGuard gpuWork{Context};
    Context.Queue->Submit({.CmdBuffers = commands});
    Context.Queue->Wait();
    auto* mapped = static_cast<uint32_t*>(readback->Map(0, 1024));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange({0, 1024});
    EXPECT_EQ(mapped[0], 62u);
    EXPECT_EQ(mapped[1], 63u);
    EXPECT_EQ(mapped[2], 64u);
    float stored;
    std::memcpy(&stored, mapped + 128, sizeof(stored));
    EXPECT_FLOAT_EQ(stored, 62.0f);
    readback->Unmap();
    EXPECT_EQ(Context.RhiValidationErrors.load(), 7u);
}

INSTANTIATE_TEST_SUITE_P(Backends, ExplicitBindingLayoutTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));

#ifdef RADRAY_ENABLE_VULKAN
// The callback context is the fake VkDevice. No production counters or shared callback state.
class VulkanLayoutProbe {
public:
    struct PoolRecord {
        VkDescriptorPool Handle;
        uint32_t Capacity;
        vector<VkDescriptorPoolSize> Sizes;
    };
    struct AllocationRecord {
        VkDescriptorPool Pool;
        vector<VkDescriptorSetLayout> Layouts;
    };
    vulkan::InstanceVulkanImpl Instance{VK_NULL_HANDLE, std::nullopt, {}, {}};
    uint32_t SetCreates{0}, SetDestroys{0}, PipelineCreates{0}, PipelineDestroys{0};
    uint32_t FreedSets{0}, PoolDestroys{0};
    uintptr_t NextHandle{1};
    bool FailSet{false}, FailPipeline{false};
    vector<VkDescriptorSetLayout> LastSets;
    unordered_map<VkDescriptorSetLayout, vector<VkDescriptorPoolSize>> LayoutCounts;
    vector<PoolRecord> Pools;
    vector<AllocationRecord> Allocations;
    // Device teardown invokes callbacks while the probe records are still alive.
    vulkan::DeviceVulkan Device{&Instance, VK_NULL_HANDLE, reinterpret_cast<VkDevice>(this)};

    VulkanLayoutProbe() {
        Device._properties.limits.maxBoundDescriptorSets = 8;
        Device._properties.limits.maxPushConstantsSize = 128;
        Device._ftb.vkGetDescriptorSetLayoutSupport = [](VkDevice, const VkDescriptorSetLayoutCreateInfo*, VkDescriptorSetLayoutSupport* support) { support->supported = VK_TRUE; };
        Device._ftb.vkCreateDescriptorSetLayout = [](VkDevice device, const VkDescriptorSetLayoutCreateInfo* info, const VkAllocationCallbacks*, VkDescriptorSetLayout* result) -> VkResult {
            auto& probe = *reinterpret_cast<VulkanLayoutProbe*>(device);
            ++probe.SetCreates;
            if (probe.FailSet) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            *result = reinterpret_cast<VkDescriptorSetLayout>(probe.NextHandle++);
            auto& counts = probe.LayoutCounts[*result];
            for (uint32_t i = 0; i < info->bindingCount; ++i) counts.push_back({info->pBindings[i].descriptorType, info->pBindings[i].descriptorCount});
            return VK_SUCCESS;
        };
        Device._ftb.vkDestroyDescriptorSetLayout = [](VkDevice device, VkDescriptorSetLayout, const VkAllocationCallbacks*) { ++reinterpret_cast<VulkanLayoutProbe*>(device)->SetDestroys; };
        Device._ftb.vkCreatePipelineLayout = [](VkDevice device, const VkPipelineLayoutCreateInfo* info, const VkAllocationCallbacks*, VkPipelineLayout* result) -> VkResult {
            auto& probe = *reinterpret_cast<VulkanLayoutProbe*>(device);
            ++probe.PipelineCreates;
            probe.LastSets.assign(info->pSetLayouts, info->pSetLayouts + info->setLayoutCount);
            if (probe.FailPipeline) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            *result = reinterpret_cast<VkPipelineLayout>(probe.NextHandle++);
            return VK_SUCCESS;
        };
        Device._ftb.vkDestroyPipelineLayout = [](VkDevice device, VkPipelineLayout, const VkAllocationCallbacks*) { ++reinterpret_cast<VulkanLayoutProbe*>(device)->PipelineDestroys; };
        Device._ftb.vkCreateDescriptorPool = [](VkDevice device, const VkDescriptorPoolCreateInfo* info, const VkAllocationCallbacks*, VkDescriptorPool* result) -> VkResult {
            auto& probe = *reinterpret_cast<VulkanLayoutProbe*>(device);
            *result = reinterpret_cast<VkDescriptorPool>(probe.NextHandle++);
            probe.Pools.push_back({*result, info->maxSets, vector<VkDescriptorPoolSize>{info->pPoolSizes, info->pPoolSizes + info->poolSizeCount}});
            return VK_SUCCESS;
        };
        Device._ftb.vkDestroyDescriptorPool = [](VkDevice device, VkDescriptorPool, const VkAllocationCallbacks*) { ++reinterpret_cast<VulkanLayoutProbe*>(device)->PoolDestroys; };
        Device._ftb.vkAllocateDescriptorSets = [](VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* sets) -> VkResult {
            auto& probe = *reinterpret_cast<VulkanLayoutProbe*>(device);
            probe.Allocations.push_back({info->descriptorPool, vector<VkDescriptorSetLayout>{info->pSetLayouts, info->pSetLayouts + info->descriptorSetCount}});
            for (uint32_t i = 0; i < info->descriptorSetCount; ++i) sets[i] = reinterpret_cast<VkDescriptorSet>(probe.NextHandle++);
            return VK_SUCCESS;
        };
        Device._ftb.vkFreeDescriptorSets = [](VkDevice device, VkDescriptorPool, uint32_t count, const VkDescriptorSet*) -> VkResult {
            reinterpret_cast<VulkanLayoutProbe*>(device)->FreedSets += count;
            return VK_SUCCESS;
        };
        Device._ftb.vkDestroyDevice = [](VkDevice, const VkAllocationCallbacks*) {};
    }
};

TEST(ExplicitLayoutOwnershipVulkan, IndependentNativeCreationBorrowingAndIdempotentDestruction) {
    VulkanLayoutProbe probe;
    const auto binding = MakeBinding(2, BindingKind::CBuffer, 0, 7, 3);
    auto group = probe.Device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    auto independent = probe.Device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    EXPECT_EQ(probe.SetCreates, 2u);
    independent->Destroy();
    independent->Destroy();
    EXPECT_EQ(probe.SetDestroys, 1u);
    const PipelineLayoutGroupDescriptor slot{92, group.get()};
    auto first = probe.Device.CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
    auto second = probe.Device.CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
    EXPECT_EQ(probe.PipelineCreates, 2u);
    EXPECT_EQ(probe.SetCreates, 4u);  // One private hole layout per pipeline, shared only among its holes.
    ASSERT_EQ(probe.LastSets.size(), 4u);
    EXPECT_EQ(probe.LastSets[0], probe.LastSets[1]);
    EXPECT_EQ(probe.LastSets[1], probe.LastSets[2]);
    first->Destroy();
    first->Destroy();
    EXPECT_EQ(probe.PipelineDestroys, 1u);
    EXPECT_EQ(probe.SetDestroys, 2u);
    EXPECT_TRUE(group->IsValid());
    EXPECT_TRUE(second->IsValid());
    second.reset();
    EXPECT_EQ(probe.PipelineDestroys, 2u);
    EXPECT_EQ(probe.SetDestroys, 3u);
    group.reset();
    EXPECT_EQ(probe.SetDestroys, 4u);
}

TEST(ExplicitLayoutOwnershipVulkan, FailedCreationReleasesOnlyResourcesAlreadyCreated) {
    VulkanLayoutProbe probe;
    const auto binding = MakeBinding(2, BindingKind::CBuffer, 0, 7, 3);
    probe.FailSet = true;
    EXPECT_FALSE(probe.Device.CreateBindingGroupLayout({std::span{&binding, 1}}));
    EXPECT_EQ(probe.SetDestroys, 0u);
    probe.FailSet = false;
    auto group = probe.Device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    const PipelineLayoutGroupDescriptor slot{4, group.get()};
    probe.FailSet = true;
    EXPECT_FALSE(probe.Device.CreatePipelineLayout({std::span{&slot, 1}, {}}));
    EXPECT_EQ(probe.PipelineCreates, 0u);
    probe.FailSet = false;
    probe.FailPipeline = true;
    EXPECT_FALSE(probe.Device.CreatePipelineLayout({std::span{&slot, 1}, {}}));
    EXPECT_EQ(probe.SetDestroys, 1u);
    EXPECT_EQ(probe.PipelineDestroys, 0u);
    EXPECT_TRUE(group->IsValid());
    group.reset();
    EXPECT_EQ(probe.SetDestroys, 2u);
}

TEST(ExplicitLayoutOwnershipVulkan, ParameterSetsAllocateOnlyTheirOwnGroupDescriptors) {
    VulkanLayoutProbe probe;
    auto viewBinding = MakeBinding(1, BindingKind::CBuffer, 0, 3, 0);
    viewBinding.Count = 2;
    BindingDescriptor materialBindings[]{MakeBinding(1, BindingKind::StructuredBuffer, 4, 5, 1), MakeBinding(2, BindingKind::Sampler, 0, 9, 1)};
    materialBindings[0].Count = 4;
    const auto objectBinding = MakeBinding(1, BindingKind::RWStructuredBuffer, 2, 7, 2);
    auto view = probe.Device.CreateBindingGroupLayout({std::span{&viewBinding, 1}}).Unwrap();
    auto material = probe.Device.CreateBindingGroupLayout({materialBindings}).Unwrap();
    auto object = probe.Device.CreateBindingGroupLayout({std::span{&objectBinding, 1}}).Unwrap();
    const PipelineLayoutGroupDescriptor groups[]{{4, view.get()}, {8, material.get()}, {12, object.get()}};
    auto pipeline = probe.Device.CreatePipelineLayout({groups, {}}).Unwrap();
    EXPECT_TRUE(probe.Allocations.empty());
    EXPECT_TRUE(probe.Pools.empty());

    const vector<VkDescriptorPoolSize> expectedCounts[]{
        {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2}},
        {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}},
        {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}}};
    BindingGroupLayout* layouts[]{view.get(), material.get(), object.get(), object.get(), object.get()};
    vector<unique_ptr<ShaderParameterSet>> sets;
    for (uint32_t i = 0; i < std::size(layouts); ++i) {
        SCOPED_TRACE(i);
        sets.push_back(probe.Device.CreateShaderParameterSet({layouts[i]}).Unwrap());
        ASSERT_EQ(probe.Allocations.size(), i + 1);
        const auto& allocation = probe.Allocations.back();
        ASSERT_EQ(allocation.Layouts.size(), 1u);
        const auto nativeLayout = static_cast<vulkan::BindingGroupLayoutVulkan*>(layouts[i])->_layout;
        EXPECT_EQ(allocation.Layouts[0], nativeLayout);
        const auto& expected = expectedCounts[std::min(i, 2u)];
        const auto& nativeCounts = probe.LayoutCounts.at(allocation.Layouts[0]);
        ASSERT_EQ(nativeCounts.size(), expected.size());
        const auto pool = std::find_if(probe.Pools.begin(), probe.Pools.end(), [&](const auto& p) { return p.Handle == allocation.Pool; });
        ASSERT_NE(pool, probe.Pools.end());
        ASSERT_GT(pool->Capacity, 0u);
        ASSERT_EQ(pool->Sizes.size(), expected.size());
        for (const auto& count : expected) {
            const auto native = std::find_if(nativeCounts.begin(), nativeCounts.end(), [&](const auto& n) { return n.type == count.type; });
            ASSERT_NE(native, nativeCounts.end());
            EXPECT_EQ(native->descriptorCount, count.descriptorCount);
            const auto capacity = std::find_if(pool->Sizes.begin(), pool->Sizes.end(), [&](const auto& n) { return n.type == count.type; });
            ASSERT_NE(capacity, pool->Sizes.end());
            EXPECT_EQ(capacity->descriptorCount, count.descriptorCount * pool->Capacity);
        }
    }
    EXPECT_EQ(probe.Pools.size(), 3u);
    sets[2]->Destroy();
    sets[2]->Destroy();
    EXPECT_EQ(probe.FreedSets, 1u);
    EXPECT_TRUE(sets[0]->IsValid());
    EXPECT_TRUE(sets[1]->IsValid());
    EXPECT_TRUE(sets[3]->IsValid());
    sets.clear();
    EXPECT_EQ(probe.FreedSets, 5u);
    probe.Device._descriptorSetAllocator.Clear();
    EXPECT_EQ(probe.PoolDestroys, 3u);
}
#endif

#ifdef RADRAY_ENABLE_D3D12

TEST(ExplicitLayoutOwnershipD3D12, NativeObjectLifetimesRespectIndependentOwners) {
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::D3D12, context, true, false)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(RenderBackend::D3D12))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    auto shutdown = MakeScopeGuard([&] {
        context.Reset();
        EXPECT_EQ(context.ValidationErrors.load(), 0u);
    });
    auto& device = *static_cast<d3d12::DeviceD3D12*>(context.Device.get());
    size_t messageStart = 0;
    const auto count = [&](D3D12_MESSAGE_ID id) {
        EXPECT_TRUE(context.Diagnostics->Checkpoint());
        return context.Diagnostics->Count(messageStart, d3d12::detail::DiagnosticSource::D3D12, static_cast<int32_t>(id));
    };
    const auto bytes = ReadStage("compute", RenderBackend::D3D12);
    ASSERT_FALSE(bytes.empty());
    auto shader = device.CreateShader({bytes, ShaderBlobCategory::DXIL, ShaderStage::Compute}).Unwrap();
    for (const bool distinctDescription : {false, true}) {
        SCOPED_TRACE(distinctDescription);
        messageStart = context.Diagnostics->CheckpointAndMark();
        const auto binding = MakeBinding(1, BindingKind::RWStructuredBuffer, 2, 6);
        auto group = device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
        const PipelineLayoutGroupDescriptor slot{3, group.get()};
        const PushConstantDescriptor unusedPush{77, 4, ShaderStage::Compute, PushConstantDescriptor::D3D12Location{9, 0}, {}};
        auto first = device.CreatePipelineLayout({std::span{&slot, 1}, {}}).Unwrap();
        const auto firstCreateMessages = count(D3D12_MESSAGE_ID_CREATE_ROOTSIGNATURE);
        auto second = device.CreatePipelineLayout({std::span{&slot, 1}, distinctDescription ? std::span{&unusedPush, 1} : std::span<const PushConstantDescriptor>{}}).Unwrap();
        ASSERT_NE(first.get(), second.get());
        d3d12::ComPtr<IUnknown> firstIdentity, secondIdentity;
        ASSERT_TRUE(SUCCEEDED(static_cast<d3d12::RootSigD3D12*>(first.get())->_rootSignature.As(&firstIdentity)));
        ASSERT_TRUE(SUCCEEDED(static_cast<d3d12::RootSigD3D12*>(second.get())->_rootSignature.As(&secondIdentity)));
        const uint32_t nativeObjects = firstIdentity.Get() == secondIdentity.Get() ? 1u : 2u;
        if (distinctDescription) EXPECT_EQ(nativeObjects, 2u);
        firstIdentity.Reset();
        secondIdentity.Reset();
        RecordProperty(distinctDescription ? "distinct_description_native_objects" : "same_description_native_objects", nativeObjects);
        const auto createMessages = count(D3D12_MESSAGE_ID_CREATE_ROOTSIGNATURE);
        ASSERT_GE(firstCreateMessages, 1u);
        if (context.GpuValidationEnabled) {
            EXPECT_GE(createMessages, nativeObjects);
        } else {
            EXPECT_EQ(createMessages, nativeObjects);
        }
        RecordProperty(distinctDescription ? "distinct_description_create_messages" : "same_description_create_messages", static_cast<int>(createMessages));
        first->Destroy();
        first->Destroy();
        EXPECT_FALSE(first->IsValid());
        EXPECT_TRUE(group->IsValid());
        ASSERT_TRUE(second->IsValid());
        EXPECT_EQ(count(D3D12_MESSAGE_ID_DESTROY_ROOTSIGNATURE), nativeObjects == 1 ? 0u : firstCreateMessages);
        {
            auto pipeline = device.CreateComputePipelineState({second.get(), {shader.get(), "CSMain"}}).Unwrap();
            auto output = device.CreateBuffer({.Size = 4, .Memory = MemoryType::Device, .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource}).Unwrap();
            auto readback = device.CreateBuffer({.Size = 4, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
            auto parameters = device.CreateShaderParameterSet({group.get()}).Unwrap();
            ASSERT_TRUE(parameters->Set(1, 0, ShaderBufferBinding{output.get(), {0, 4}, 4}));
            ASSERT_TRUE(parameters->FlushWrites());
            auto allocator = device.CreateCommandAllocator(context.Queue).Unwrap();
            auto command = device.CreateCommandBuffer(allocator.get()).Unwrap();
            command->Begin();
            const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{output.get(), BufferState::Undefined, BufferState::UnorderedAccess};
            command->ResourceBarrier(std::span{&toUav, 1});
            auto encoder = command->BeginComputePass().Unwrap();
            encoder->BindComputePipelineState(pipeline.get());
            encoder->BindShaderParameterSet(3, parameters.get());
            encoder->Dispatch(1, 1, 1);
            command->EndComputePass(std::move(encoder));
            const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{output.get(), BufferState::UnorderedAccess, BufferState::CopySource};
            command->ResourceBarrier(std::span{&toCopy, 1});
            command->CopyBufferToBuffer(readback.get(), 0, output.get(), 0, 4);
            command->End();
            CommandBuffer* commands[]{command.get()};
            test::GpuWorkGuard gpuWork{context};
            context.Queue->Submit({.CmdBuffers = commands});
            context.Queue->Wait();
            const auto* value = static_cast<const uint32_t*>(readback->Map(0, 4));
            ASSERT_NE(value, nullptr);
            readback->InvalidateMappedRange({0, 4});
            EXPECT_EQ(*value, 0x12345678u);
            readback->Unmap();
        }
        second->Destroy();
        second->Destroy();
        EXPECT_FALSE(second->IsValid());
        EXPECT_TRUE(group->IsValid());
        first.reset();
        second.reset();
        context.Queue->Wait();
        EXPECT_EQ(count(D3D12_MESSAGE_ID_DESTROY_ROOTSIGNATURE), createMessages);
    }
}

TEST(ExplicitLayoutOwnershipD3D12, ParameterSetsAllocateOnlyTheirOwnGroupDescriptors) {
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::D3D12, context, true, false)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(RenderBackend::D3D12))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    auto& device = *context.Device;
    auto viewBinding = MakeBinding(1, BindingKind::CBuffer, 0, 3, 0);
    viewBinding.Count = 2;
    BindingDescriptor materialBindings[]{MakeBinding(1, BindingKind::StructuredBuffer, 4, 5, 1), MakeBinding(2, BindingKind::Sampler, 0, 9, 1)};
    materialBindings[0].Count = 4;
    const auto objectBinding = MakeBinding(1, BindingKind::RWStructuredBuffer, 2, 7, 2);
    auto view = device.CreateBindingGroupLayout({std::span{&viewBinding, 1}}).Unwrap();
    auto material = device.CreateBindingGroupLayout({materialBindings}).Unwrap();
    auto object = device.CreateBindingGroupLayout({std::span{&objectBinding, 1}}).Unwrap();
    const PipelineLayoutGroupDescriptor groups[]{{4, view.get()}, {8, material.get()}, {12, object.get()}};
    auto pipeline = device.CreatePipelineLayout({groups, {}}).Unwrap();
    BindingGroupLayout* layouts[]{view.get(), material.get(), object.get(), object.get(), object.get()};
    const uint32_t expectedDescriptors[]{2, 5, 1, 1, 1};
    const uint32_t expectedTables[]{1, 2, 1, 1, 1};
    vector<unique_ptr<ShaderParameterSet>> sets;
    for (uint32_t i = 0; i < std::size(layouts); ++i) {
        SCOPED_TRACE(i);
        sets.push_back(device.CreateShaderParameterSet({layouts[i]}).Unwrap());
        const auto& native = *static_cast<d3d12::ShaderParameterSetD3D12*>(sets.back().get());
        ASSERT_EQ(native._tables.size(), expectedTables[i]);
        EXPECT_TRUE(native._rootBindings.empty());
        uint32_t cpuCount = 0, gpuCount = 0;
        for (const auto& table : native._tables) {
            ASSERT_TRUE(table.Mirror.IsValid());
            ASSERT_TRUE(table.Descriptors.IsValid());
            cpuCount += table.Mirror.GetLength();
            gpuCount += table.Descriptors.GetLength();
        }
        EXPECT_EQ(cpuCount, expectedDescriptors[i]);
        EXPECT_EQ(gpuCount, expectedDescriptors[i]);
    }
    sets[2]->Destroy();
    sets[2]->Destroy();
    EXPECT_TRUE(sets[0]->IsValid());
    EXPECT_TRUE(sets[1]->IsValid());
    EXPECT_TRUE(sets[3]->IsValid());
    sets.clear();
    pipeline.reset();
    object.reset();
    material.reset();
    view.reset();
    context.Reset();
    EXPECT_EQ(context.ValidationErrors.load(), 0u);
}

TEST(DescriptorDirtyRangesD3D12Test, CoalescesAndNeverCopiesUninitializedGaps) {
    d3d12::DescriptorDirtyRangesD3D12 dirty;
    vector<uint8_t> initialized(128, 1);
    dirty.Add(4, 2);
    dirty.Add(0, 2);
    dirty.Add(2, 2);
    ASSERT_EQ(dirty.Ranges.size(), 1u);
    EXPECT_EQ(dirty.Ranges[0], (d3d12::DescriptorDirtyRangeD3D12{0, 6}));
    ASSERT_TRUE(dirty.GetCopySpan(initialized).has_value());
    dirty.Ranges.clear();
    dirty.Add(0);
    dirty.Add(3);
    ASSERT_TRUE(dirty.GetCopySpan(initialized).has_value());  // Exactly 50%.
    initialized[1] = 0;
    EXPECT_FALSE(dirty.GetCopySpan(initialized).has_value());
    initialized[1] = 1;
    dirty.Ranges.clear();
    for (uint32_t i = 0; i < 16; ++i) dirty.Add(i * 7);
    EXPECT_FALSE(dirty.GetCopySpan(initialized).has_value());
    dirty.Add(112);
    EXPECT_TRUE(dirty.GetCopySpan(initialized).has_value());
    initialized[50] = 0;
    EXPECT_FALSE(dirty.GetCopySpan(initialized).has_value());
    dirty.Ranges.clear();
    dirty.Add(std::numeric_limits<uint32_t>::max() - 1);
    EXPECT_FALSE(dirty.GetCopySpan(initialized).has_value());
}

#endif
}  // namespace
}  // namespace radray::render
