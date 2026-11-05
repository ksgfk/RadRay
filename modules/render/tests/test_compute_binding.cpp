#include "gpu_test_fixture.h"

#include <algorithm>

namespace radray::render {
namespace {

// Bindings of data/compute.hlsl as (D3D12 register, space) and (Vulkan set, binding).
struct ShaderSlot {
    uint32_t Register, Space, Set, Binding;
};
constexpr ShaderSlot OutputSlot{0, 0, 0, 0};
constexpr ShaderSlot InputSlot{0, 0, 0, 1};
constexpr ShaderSlot DataSlot{0, 0, 0, 2};
constexpr ShaderSlot ImageSlot{1, 0, 0, 3};
constexpr ShaderSlot SamplerSlot{0, 0, 0, 4};
constexpr ShaderSlot MaterialSlot{0, 1, 1, 0};
constexpr ShaderSlot ObjectSlot{0, 2, 2, 0};
constexpr uint32_t PushRegister = 1;

struct Constants {
    uint32_t Value, Slot;
};

BindingDescriptor MakeBinding(uint32_t id, BindingKind kind, ShaderSlot slot, bool dynamic = false) {
    return {.BindingId = id,
            .Kind = kind,
            .Count = 1,
            .Stages = ShaderStage::Compute,
            .D3D12 = D3D12BindingDescriptor{GetBindingRegisterClass(kind), slot.Register, slot.Space,
                                            dynamic ? D3D12BufferPlacement::RootDescriptor : D3D12BufferPlacement::Table},
            .Vulkan = VulkanBindingDescriptor{slot.Set, slot.Binding,
                                              dynamic ? VulkanBufferDescriptorPlacement::Dynamic : VulkanBufferDescriptorPlacement::Regular}};
}

struct Kernel {
    unique_ptr<Shader> Code;
    unique_ptr<PipelineLayout> Layout;
    unique_ptr<ComputePipelineState> Pipeline;
};

class ComputeBindingTest : public test::BackendTest {
protected:
    static constexpr uint32_t OutputId = 0;
    static constexpr uint32_t OutputCount = 4;

    Kernel CreateKernel(std::string_view entry, std::span<const PipelineLayoutGroupDescriptor> groups,
                        std::span<const PushConstantDescriptor> pushes = {}) {
        auto& device = GetDevice();
        Kernel kernel;
        kernel.Code = test::LoadShader(device, "compute", entry, ShaderStage::Compute).Unwrap();
        kernel.Layout = device.CreatePipelineLayout({groups, pushes}).Unwrap();
        kernel.Pipeline = device.CreateComputePipelineState({kernel.Layout.get(), {kernel.Code.get(), entry}}).Unwrap();
        return kernel;
    }

    unique_ptr<Buffer> CreateOutput() {
        return GetDevice().CreateBuffer({.Size = OutputCount * sizeof(uint32_t), .Memory = MemoryType::Device,
                                         .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource})
            .Unwrap();
    }

    unique_ptr<Buffer> CreateInput(std::span<const uint32_t> values, BufferUses usage) {
        return test::CreateUploadBuffer(GetDevice(), std::as_bytes(values), usage).Unwrap();
    }

    unique_ptr<ShaderParameterSet> CreateSet(BindingGroupLayout& layout) {
        return GetDevice().CreateShaderParameterSet({&layout}).Unwrap();
    }

    vector<uint32_t> ReadOutput(Buffer& output) {
        return test::FromBytes<uint32_t>(test::ReadBuffer(GetDevice(), output, BufferState::UnorderedAccess, OutputCount * sizeof(uint32_t)));
    }

    static ShaderBufferBinding OutputBinding(Buffer& output) {
        return {&output, {0, OutputCount * sizeof(uint32_t)}, sizeof(uint32_t)};
    }
};

TEST_P(ComputeBindingTest, ConstantBufferIsVisible) {
    auto& device = GetDevice();
    const BindingDescriptor bindings[]{MakeBinding(OutputId, BindingKind::RWStructuredBuffer, OutputSlot),
                                       MakeBinding(1, BindingKind::CBuffer, DataSlot)};
    auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
    const PipelineLayoutGroupDescriptor groups[]{{0, group.get()}};
    auto kernel = CreateKernel("ReadConstants", groups);
    auto output = CreateOutput();
    vector<uint32_t> constants(64, 0);
    constants[0] = 42;
    constants[1] = 2;
    auto data = CreateInput(constants, BufferUse::CBuffer);
    auto set = CreateSet(*group);
    ASSERT_TRUE(set->Set(OutputId, 0, OutputBinding(*output)));
    ASSERT_TRUE(set->Set(1, 0, ShaderBufferBinding{data.get(), {0, 256}, 0}));
    ASSERT_TRUE(set->FlushWrites());

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = data.get(), .Before = BufferState::HostWrite, .After = BufferState::CBuffer},
                             BarrierBufferDescriptor{.Target = output.get(), .Before = BufferState::Undefined, .After = BufferState::UnorderedAccess}});
    auto pass = commands.BeginComputePass().Unwrap();
    pass->BindComputePipelineState(kernel.Pipeline.get());
    pass->BindShaderParameterSet(0, set.get());
    pass->Dispatch(1, 1, 1);
    commands.EndComputePass(std::move(pass));
    test::SubmitAndWait(device, recording);

    EXPECT_EQ(ReadOutput(*output)[2], 42u);
}

TEST_P(ComputeBindingTest, PushConstantsAreVisible) {
    auto& device = GetDevice();
    const BindingDescriptor binding = MakeBinding(OutputId, BindingKind::RWStructuredBuffer, OutputSlot);
    auto group = device.CreateBindingGroupLayout({std::span{&binding, 1}}).Unwrap();
    const PipelineLayoutGroupDescriptor groups[]{{0, group.get()}};
    const PushConstantDescriptor push{.PushId = 5, .Size = 8, .Stages = ShaderStage::Compute,
                                      .D3D12 = PushConstantDescriptor::D3D12Location{PushRegister, 0},
                                      .Vulkan = PushConstantDescriptor::VulkanLocation{0}};
    auto kernel = CreateKernel("ReadPushConstants", groups, std::span{&push, 1});
    auto output = CreateOutput();
    auto set = CreateSet(*group);
    ASSERT_TRUE(set->Set(OutputId, 0, OutputBinding(*output)));
    ASSERT_TRUE(set->FlushWrites());

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = output.get(), .Before = BufferState::Undefined, .After = BufferState::UnorderedAccess}});
    auto pass = commands.BeginComputePass().Unwrap();
    pass->BindComputePipelineState(kernel.Pipeline.get());
    pass->BindShaderParameterSet(0, set.get());
    const uint32_t values[]{11, 22};
    EXPECT_TRUE(pass->SetPushConstants(5, 0, std::as_bytes(std::span{values})));
    pass->Dispatch(1, 1, 1);
    commands.EndComputePass(std::move(pass));
    test::SubmitAndWait(device, recording);

    const auto result = ReadOutput(*output);
    EXPECT_EQ(result[0], 11u);
    EXPECT_EQ(result[1], 22u);
}

TEST_P(ComputeBindingTest, ShaderReadsResourceBufferAndWritesUnorderedAccessBuffer) {
    auto& device = GetDevice();
    const BindingDescriptor bindings[]{MakeBinding(OutputId, BindingKind::RWStructuredBuffer, OutputSlot),
                                       MakeBinding(1, BindingKind::StructuredBuffer, InputSlot)};
    auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
    const PipelineLayoutGroupDescriptor groups[]{{0, group.get()}};
    auto kernel = CreateKernel("TransformInput", groups);
    auto output = CreateOutput();
    const uint32_t values[]{1, 2, 3, 4};
    auto input = CreateInput(values, BufferUse::Resource);
    auto set = CreateSet(*group);
    ASSERT_TRUE(set->Set(OutputId, 0, OutputBinding(*output)));
    ASSERT_TRUE(set->Set(1, 0, ShaderBufferBinding{input.get(), {0, sizeof(values)}, sizeof(uint32_t)}));
    ASSERT_TRUE(set->FlushWrites());

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = input.get(), .Before = BufferState::HostWrite, .After = BufferState::ShaderRead},
                             BarrierBufferDescriptor{.Target = output.get(), .Before = BufferState::Undefined, .After = BufferState::UnorderedAccess}});
    auto pass = commands.BeginComputePass().Unwrap();
    pass->BindComputePipelineState(kernel.Pipeline.get());
    pass->BindShaderParameterSet(0, set.get());
    pass->Dispatch(1, 1, 1);
    commands.EndComputePass(std::move(pass));
    test::SubmitAndWait(device, recording);

    EXPECT_EQ(ReadOutput(*output), (vector<uint32_t>{3, 5, 7, 9}));
}

TEST_P(ComputeBindingTest, TextureIsSampledThroughSampler) {
    auto& device = GetDevice();
    const BindingDescriptor bindings[]{MakeBinding(OutputId, BindingKind::RWStructuredBuffer, OutputSlot),
                                       MakeBinding(1, BindingKind::Texture, ImageSlot),
                                       MakeBinding(2, BindingKind::Sampler, SamplerSlot)};
    auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
    const PipelineLayoutGroupDescriptor groups[]{{0, group.get()}};
    auto kernel = CreateKernel("SampleImage", groups);
    auto output = CreateOutput();
    auto texture = device.CreateTexture({.Dim = TextureDimension::Dim2D, .Width = 2, .Height = 2, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1,
                                         .Format = TextureFormat::RGBA8_UNORM, .Usage = TextureUse::Resource | TextureUse::CopyDestination})
                       .Unwrap();
    const uint32_t texels[]{0xff302010, 0xff605040, 0xff908070, 0xffc0b0a0};
    test::UploadTexture(device, *texture, TextureState::Undefined, std::as_bytes(std::span{texels}));
    auto view = device.CreateTextureView({texture.get(), TextureDimension::Dim2D, TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, TextureViewUsage::Resource}).Unwrap();
    auto sampler = device.CreateSampler({.AddressS = AddressMode::ClampToEdge, .AddressT = AddressMode::ClampToEdge, .AddressR = AddressMode::ClampToEdge,
                                         .MinFilter = FilterMode::Nearest, .MagFilter = FilterMode::Nearest, .MipmapFilter = FilterMode::Nearest})
                       .Unwrap();
    auto set = CreateSet(*group);
    ASSERT_TRUE(set->Set(OutputId, 0, OutputBinding(*output)));
    ASSERT_TRUE(set->Set(1, 0, view.get()));
    ASSERT_TRUE(set->Set(2, 0, sampler.get()));
    ASSERT_TRUE(set->FlushWrites());

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierTextureDescriptor{.Target = texture.get(), .Before = TextureState::CopyDestination, .After = TextureState::ShaderRead},
                             BarrierBufferDescriptor{.Target = output.get(), .Before = BufferState::Undefined, .After = BufferState::UnorderedAccess}});
    auto pass = commands.BeginComputePass().Unwrap();
    pass->BindComputePipelineState(kernel.Pipeline.get());
    pass->BindShaderParameterSet(0, set.get());
    pass->Dispatch(1, 1, 1);
    commands.EndComputePass(std::move(pass));
    test::SubmitAndWait(device, recording);

    const auto result = ReadOutput(*output);
    EXPECT_EQ(result[0], texels[0]);
    EXPECT_EQ(result[1], texels[3]);
}

TEST_P(ComputeBindingTest, RebindingOneGroupKeepsOtherGroupsBound) {
    auto& device = GetDevice();
    const BindingDescriptor outputBinding = MakeBinding(OutputId, BindingKind::RWStructuredBuffer, OutputSlot);
    const BindingDescriptor materialBinding = MakeBinding(0, BindingKind::StructuredBuffer, MaterialSlot);
    const BindingDescriptor objectBinding = MakeBinding(0, BindingKind::StructuredBuffer, ObjectSlot);
    auto outputGroup = device.CreateBindingGroupLayout({std::span{&outputBinding, 1}}).Unwrap();
    auto materialGroup = device.CreateBindingGroupLayout({std::span{&materialBinding, 1}}).Unwrap();
    auto objectGroup = device.CreateBindingGroupLayout({std::span{&objectBinding, 1}}).Unwrap();
    const PipelineLayoutGroupDescriptor groups[]{{0, outputGroup.get()}, {1, materialGroup.get()}, {2, objectGroup.get()}};
    auto kernel = CreateKernel("CombineGroups", groups);
    auto output = CreateOutput();
    const uint32_t material[]{7};
    const uint32_t firstObject[]{3, 0};
    const uint32_t secondObject[]{5, 1};
    auto materialData = CreateInput(material, BufferUse::Resource);
    auto firstObjectData = CreateInput(firstObject, BufferUse::Resource);
    auto secondObjectData = CreateInput(secondObject, BufferUse::Resource);
    auto outputSet = CreateSet(*outputGroup);
    auto materialSet = CreateSet(*materialGroup);
    auto firstObjectSet = CreateSet(*objectGroup);
    auto secondObjectSet = CreateSet(*objectGroup);
    ASSERT_TRUE(outputSet->Set(OutputId, 0, OutputBinding(*output)));
    ASSERT_TRUE(materialSet->Set(0, 0, ShaderBufferBinding{materialData.get(), {0, sizeof(material)}, sizeof(uint32_t)}));
    ASSERT_TRUE(firstObjectSet->Set(0, 0, ShaderBufferBinding{firstObjectData.get(), {0, sizeof(firstObject)}, sizeof(uint32_t)}));
    ASSERT_TRUE(secondObjectSet->Set(0, 0, ShaderBufferBinding{secondObjectData.get(), {0, sizeof(secondObject)}, sizeof(uint32_t)}));
    for (auto* set : {outputSet.get(), materialSet.get(), firstObjectSet.get(), secondObjectSet.get()}) ASSERT_TRUE(set->FlushWrites());

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = materialData.get(), .Before = BufferState::HostWrite, .After = BufferState::ShaderRead},
                             BarrierBufferDescriptor{.Target = firstObjectData.get(), .Before = BufferState::HostWrite, .After = BufferState::ShaderRead},
                             BarrierBufferDescriptor{.Target = secondObjectData.get(), .Before = BufferState::HostWrite, .After = BufferState::ShaderRead},
                             BarrierBufferDescriptor{.Target = output.get(), .Before = BufferState::Undefined, .After = BufferState::UnorderedAccess}});
    auto pass = commands.BeginComputePass().Unwrap();
    pass->BindComputePipelineState(kernel.Pipeline.get());
    pass->BindShaderParameterSet(0, outputSet.get());
    pass->BindShaderParameterSet(1, materialSet.get());
    pass->BindShaderParameterSet(2, firstObjectSet.get());
    pass->Dispatch(1, 1, 1);
    pass->BindShaderParameterSet(2, secondObjectSet.get());
    pass->Dispatch(1, 1, 1);
    commands.EndComputePass(std::move(pass));
    test::SubmitAndWait(device, recording);

    const auto result = ReadOutput(*output);
    EXPECT_EQ(result[0], 703u);
    EXPECT_EQ(result[1], 705u);
}

TEST_P(ComputeBindingTest, DynamicOffsetSelectsConstantBufferSlice) {
    auto& device = GetDevice();
    const BindingDescriptor bindings[]{MakeBinding(OutputId, BindingKind::RWStructuredBuffer, OutputSlot),
                                       MakeBinding(1, BindingKind::CBuffer, DataSlot, true)};
    auto group = device.CreateBindingGroupLayout({bindings}).Unwrap();
    const PipelineLayoutGroupDescriptor groups[]{{0, group.get()}};
    auto kernel = CreateKernel("ReadConstants", groups);
    auto output = CreateOutput();
    const auto stride = static_cast<uint32_t>(std::max<uint64_t>(256, device.GetCapabilities().Limits.CBufferOffsetAlignment));
    vector<uint32_t> slices(2 * stride / sizeof(uint32_t), 0);
    const Constants first{10, 0}, second{20, 1};
    std::memcpy(slices.data(), &first, sizeof(first));
    std::memcpy(slices.data() + stride / sizeof(uint32_t), &second, sizeof(second));
    auto data = CreateInput(slices, BufferUse::CBuffer);
    auto set = CreateSet(*group);
    ASSERT_TRUE(set->Set(OutputId, 0, OutputBinding(*output)));
    ASSERT_TRUE(set->Set(1, 0, ShaderBufferBinding{data.get(), {0, stride}, 0}));
    ASSERT_TRUE(set->FlushWrites());

    auto recording = test::BeginRecording(device);
    auto& commands = *recording.Commands;
    test::Barrier(commands, {BarrierBufferDescriptor{.Target = data.get(), .Before = BufferState::HostWrite, .After = BufferState::CBuffer},
                             BarrierBufferDescriptor{.Target = output.get(), .Before = BufferState::Undefined, .After = BufferState::UnorderedAccess}});
    auto pass = commands.BeginComputePass().Unwrap();
    pass->BindComputePipelineState(kernel.Pipeline.get());
    for (const uint32_t offset : {0u, stride}) {
        const ShaderParameterDynamicOffset dynamicOffset{.BindingId = 1, .ArrayIndex = 0, .Offset = offset};
        pass->BindShaderParameterSet(0, set.get(), std::span{&dynamicOffset, 1});
        pass->Dispatch(1, 1, 1);
    }
    commands.EndComputePass(std::move(pass));
    test::SubmitAndWait(device, recording);

    const auto result = ReadOutput(*output);
    EXPECT_EQ(result[0], 10u);
    EXPECT_EQ(result[1], 20u);
}

INSTANTIATE_TEST_SUITE_P(Backends, ComputeBindingTest, testing::ValuesIn(test::AllBackends), test::BackendParamName);

}  // namespace
}  // namespace radray::render
