#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <radray/render/shader_layout.h>
#include <radray/shader/shader_artifact.h>

#include "gpu_test_fixture.h"
#include "shader_contract_fixtures.h"

// M3 覆盖: Vulkan native chain 只从 ResolvedVulkanLayout 建立。这些测试要真设备,
// 因为 immutable sampler 与 empty set hole 只有在 vkCreateDescriptorSetLayout /
// vkCreatePipelineLayout 真正接受之后才算成立。
#if defined(RADRAY_ENABLE_VULKAN)

namespace radray::render {
namespace {

using vulkan::CastVkObject;
using vulkan::DeviceVulkan;
using vulkan::ShaderParameterSetLayoutEntryVulkan;

constexpr uint64_t kFixtureToolchainIdentity = 0x0000000001090211ull;

vector<byte> ReadBinary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    vector<byte> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return file.good() || file.eof() ? data : vector<byte>{};
}

std::optional<size_t> FindFixtureIndex(std::string_view name) {
    const auto fixtures = test::GetShaderContractFixtures();
    for (size_t index = 0; index < fixtures.size(); ++index) {
        if (fixtures[index].Name == name) {
            return index;
        }
    }
    return std::nullopt;
}

std::optional<ResolvedVulkanLayout> ResolveFixture(std::string_view name) {
    const std::optional<size_t> index = FindFixtureIndex(name);
    if (!index.has_value()) {
        return std::nullopt;
    }
    const std::filesystem::path path =
        std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/shader_artifacts" /
        (string{name} + ".spirv.bin");
    const vector<byte> blob = ReadBinary(path);
    if (blob.empty()) {
        return std::nullopt;
    }
    const auto artifact = shader::DecodeSpirvShaderArtifact(
        blob,
        shader::ShaderArtifactDecodeOptions{
            .Target = shader::ShaderTarget::SPIRV,
            .ExpectedGpuArtifact = test::ExpectedGpuArtifact(index.value(), shader::ShaderTarget::SPIRV),
            .ExpectedToolchainIdentity = kFixtureToolchainIdentity},
        nullptr);
    if (!artifact.has_value()) {
        return std::nullopt;
    }
    return ResolveVulkanLayout(artifact.value());
}

// Hand-built layouts let a test describe shapes no fixture happens to produce: several dynamic
// descriptors inside one set, a set hole in the middle, and placements that must be rejected. The
// resolved layout is a plain owning value, so building one is the same thing resolution produces.
ResolvedVulkanBinding MakeBinding(
    std::string_view name,
    shader::ShaderBindingKind kind,
    uint32_t set,
    uint32_t binding,
    VulkanBufferDescriptorPlacement placement = VulkanBufferDescriptorPlacement::Regular) {
    ResolvedVulkanBinding result{};
    result.Name = string{name};
    result.LogicalKind = kind;
    result.Set = set;
    result.Binding = binding;
    result.Count = 1;
    result.Stages = ShaderStages{ShaderStage::Vertex} | ShaderStage::Pixel;
    result.Placement = placement;
    return result;
}

// set 0: b0 dynamic cbuffer, b1 dynamic storage buffer, b2 sampled image, b3 uniform texel buffer.
// set 1 stays empty on purpose. set 2: b0 regular cbuffer.
ResolvedVulkanLayout MakeMixedLayout() {
    ResolvedVulkanLayout layout{};
    layout.Bindings = {
        MakeBinding(
            "DynamicView",
            shader::ShaderBindingKind::CBuffer,
            0,
            0,
            VulkanBufferDescriptorPlacement::Dynamic),
        MakeBinding(
            "DynamicItems",
            shader::ShaderBindingKind::StructuredBuffer,
            0,
            1,
            VulkanBufferDescriptorPlacement::Dynamic),
        MakeBinding("Albedo", shader::ShaderBindingKind::Texture, 0, 2),
        MakeBinding("Palette", shader::ShaderBindingKind::TypedBuffer, 0, 3),
        MakeBinding("Object", shader::ShaderBindingKind::CBuffer, 2, 0),
    };
    layout.SetCount = 3;
    layout.DynamicOffsetOrder = {0, 1};
    return layout;
}

struct VulkanDeviceFixture : ::testing::Test {
    test::DeviceContext Context;
    bool Available{false};
    DeviceVulkan* VkDevice{nullptr};

    void SetUp() override {
        Available = test::TryCreateDevice(RenderBackend::Vulkan, Context, true);
        if (Available) {
            // The resolved-layout overload is Vulkan specific: it is not on the shared Device
            // interface, because a D3D12 layout is a different resolved type.
            VkDevice = static_cast<DeviceVulkan*>(Context.Device.get());
        }
    }
};

}  // namespace

TEST_F(VulkanDeviceFixture, LogicalKindDecidesTheNativeDescriptorType) {
    if (!Available) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    const ResolvedVulkanLayout layout = MakeMixedLayout();
    auto pipelineLayout = VkDevice->CreatePipelineLayout(layout);
    ASSERT_TRUE(pipelineLayout.HasValue());
    auto* native = CastVkObject(pipelineLayout.Get());

    // A set hole keeps its index: dropping the empty set would renumber set 2 and silently point the
    // shader's set 2 at a different layout.
    ASSERT_EQ(native->_parameterSetLayouts.size(), 3u);
    ASSERT_EQ(native->_setLayoutRefs.size(), 3u);
    EXPECT_TRUE(native->_parameterSetLayouts[1].empty());
    ASSERT_EQ(native->_parameterSetLayouts[0].size(), 4u);
    ASSERT_EQ(native->_parameterSetLayouts[2].size(), 1u);

    // Uniform vs storage, dynamic vs regular, and texel buffer vs sampled image all have to stay
    // apart: collapsing any pair would bind the wrong descriptor class at draw time.
    const auto& first = native->_parameterSetLayouts[0];
    EXPECT_EQ(first[0].DescriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC);
    EXPECT_EQ(first[1].DescriptorType, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC);
    EXPECT_EQ(first[2].DescriptorType, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    EXPECT_EQ(first[3].DescriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER);
    EXPECT_EQ(native->_parameterSetLayouts[2][0].DescriptorType, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);

    // The dynamic order is the resolved order projected onto each set, not a re-derivation.
    ASSERT_EQ(native->_dynamicEntryOrder.size(), 3u);
    EXPECT_EQ(native->_dynamicEntryOrder[0], (vector<uint32_t>{0u, 1u}));
    EXPECT_TRUE(native->_dynamicEntryOrder[1].empty());
    EXPECT_TRUE(native->_dynamicEntryOrder[2].empty());

    // Names resolve against the layout that produced them, and only against that one. The handle is
    // opaque, so what is pinned here is the record it names.
    const BindingHandle albedo = native->FindBinding("Albedo");
    ASSERT_TRUE(albedo.IsValid());
    const auto albedoRecord = FindBackendBindingRecord(
        native->_bindingNames, native->_bindingGeneration, albedo);
    ASSERT_TRUE(albedoRecord.HasValue());
    EXPECT_EQ(albedoRecord.Get()->Kind, BackendBindingRecordKind::Descriptor);
    EXPECT_EQ(albedoRecord.Get()->Location.Group, 0u);
    EXPECT_EQ(albedoRecord.Get()->Location.Binding, 2u);
    EXPECT_FALSE(native->FindBinding("NotDeclared").IsValid());
}

TEST_F(VulkanDeviceFixture, IllegalDynamicPlacementFailsNativeCreation) {
    if (!Available) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    // A texture has no dynamic descriptor type. Reaching native creation with one means the wire or
    // the resolve produced something the backend cannot express, so it must fail rather than pick a
    // nearby descriptor type.
    ResolvedVulkanLayout layout{};
    layout.Bindings = {MakeBinding(
        "Albedo",
        shader::ShaderBindingKind::Texture,
        0,
        0,
        VulkanBufferDescriptorPlacement::Dynamic)};
    layout.SetCount = 1;
    layout.DynamicOffsetOrder = {0};
    EXPECT_FALSE(VkDevice->CreatePipelineLayout(layout).HasValue());

    // The dynamic order has to name exactly the dynamic bindings: a short, long or mistargeted order
    // would shift every later offset onto the wrong buffer.
    ResolvedVulkanLayout missingOrder = MakeMixedLayout();
    missingOrder.DynamicOffsetOrder = {0};
    EXPECT_FALSE(VkDevice->CreatePipelineLayout(missingOrder).HasValue());

    ResolvedVulkanLayout wrongOrder = MakeMixedLayout();
    wrongOrder.DynamicOffsetOrder = {0, 2};
    EXPECT_FALSE(VkDevice->CreatePipelineLayout(wrongOrder).HasValue());

    ResolvedVulkanLayout outOfRangeSet = MakeMixedLayout();
    outOfRangeSet.SetCount = 1;
    EXPECT_FALSE(VkDevice->CreatePipelineLayout(outOfRangeSet).HasValue());
}

TEST_F(VulkanDeviceFixture, PolicySamplerBecomesAnImmutableSamplerWithEmptySetHoles) {
    if (!Available) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    const std::optional<ResolvedVulkanLayout> layout = ResolveFixture("shadow_static_sampler");
    ASSERT_TRUE(layout.has_value());
    // The fixture binds at set 4, so sets 0..3 are holes the layout still has to materialize.
    ASSERT_EQ(layout->SetCount, 5u);
    ASSERT_EQ(layout->ImmutableSamplers.size(), 1u);
    EXPECT_EQ(layout->ImmutableSamplers[0].CompareEnable, 1u);

    auto pipelineLayout = VkDevice->CreatePipelineLayout(layout.value());
    ASSERT_TRUE(pipelineLayout.HasValue());
    auto* native = CastVkObject(pipelineLayout.Get());
    ASSERT_EQ(native->_setLayoutRefs.size(), 5u);
    for (uint32_t setIndex = 0; setIndex < 4; ++setIndex) {
        EXPECT_TRUE(native->_parameterSetLayouts[setIndex].empty()) << setIndex;
    }
    ASSERT_EQ(native->_immutableSamplers.size(), 1u);
    EXPECT_NE(native->_immutableSamplers[0], VK_NULL_HANDLE);

    const auto& entries = native->_parameterSetLayouts[4];
    const auto sampler = std::find_if(
        entries.begin(),
        entries.end(),
        [](const ShaderParameterSetLayoutEntryVulkan& value) noexcept {
            return value.LogicalKind == shader::ShaderBindingKind::Sampler;
        });
    ASSERT_NE(sampler, entries.end());
    EXPECT_EQ(sampler->DescriptorType, VK_DESCRIPTOR_TYPE_SAMPLER);
    EXPECT_TRUE(sampler->HasImmutableSampler());

    // The policy already fixed this slot, so a caller has nothing to write there.
    auto parameterSet = VkDevice->CreateShaderParameterSet(
        ShaderParameterSetDescriptor{.Layout = pipelineLayout.Get(), .GroupIndex = 4});
    ASSERT_TRUE(parameterSet.HasValue());
    const BindingHandle samplerHandle = native->FindBinding("ShadowSampler");
    ASSERT_TRUE(samplerHandle.IsValid());
    auto ownSampler = Context.Device->CreateSampler(SamplerDescriptor{});
    ASSERT_TRUE(ownSampler.HasValue());
    EXPECT_FALSE(parameterSet.Get()->Set(samplerHandle, 0, ownSampler.Get()));

    // The texture in the same set is an ordinary sampled image and stays writable.
    const BindingHandle textureHandle = native->FindBinding("ShadowTexture");
    ASSERT_TRUE(textureHandle.IsValid());
    const auto texture = test::MakeRenderTarget(
        Context.Device.get(),
        TextureFormat::R32_FLOAT,
        4,
        4,
        TextureUse::Resource);
    ASSERT_TRUE(texture.has_value());
    TextureViewDescriptor viewDesc{
        .Target = texture->Tex.get(),
        .Dim = TextureDimension::Dim2D,
        .Format = TextureFormat::R32_FLOAT,
        .Range = SubresourceRange{0, 1, 0, 1},
        .Usage = TextureViewUsage::Resource};
    auto resourceView = Context.Device->CreateTextureView(viewDesc);
    ASSERT_TRUE(resourceView.HasValue());
    EXPECT_TRUE(parameterSet.Get()->Set(textureHandle, 0, resourceView.Get()));
    EXPECT_TRUE(parameterSet.Get()->FlushWrites());
}

// Push constants reach the native layout through the same handle table as descriptor bindings, and
// only the Vulkan side can prove that a push write lands on a real VkPushConstantRange. The `compute`
// fixture declares no push block, so one is added to the resolved layout the way a pipeline recipe
// would ask for it: a Vulkan pipeline layout may declare a range the shader never reads.
TEST_F(VulkanDeviceFixture, PushHandleWritesPushConstantsAndRejectsMisuse) {
    if (!Available) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    std::optional<ResolvedVulkanLayout> resolved = ResolveFixture("compute");
    ASSERT_TRUE(resolved.has_value());
    ASSERT_EQ(resolved->Bindings.size(), 1u);
    ASSERT_FALSE(resolved->PushBlock.has_value());
    // A descriptor declared at set 0 binding 0 collides with the push block's (space 0, register 0)
    // location, so rejecting a descriptor handle here can only come from the record kind and not from
    // a location mismatch. The compute shader never reads it; a Vulkan set layout may declare more
    // bindings than the shader statically uses.
    ResolvedVulkanBinding unusedBinding{};
    unusedBinding.Name = "UnusedAtPushLocation";
    unusedBinding.LogicalKind = shader::ShaderBindingKind::CBuffer;
    unusedBinding.Set = 0;
    unusedBinding.Binding = 0;
    unusedBinding.Count = 1;
    unusedBinding.Stages = ShaderStages{ShaderStage::Compute};
    unusedBinding.Placement = VulkanBufferDescriptorPlacement::Regular;
    resolved->Bindings.insert(resolved->Bindings.begin(), std::move(unusedBinding));
    resolved->PushBlock = ResolvedPushConstantBlock{
        .Name = "TestConstants",
        .RegisterSpace = 0,
        .Register = 0,
        .Offset = 0,
        .Size = 16,
        .Stages = ShaderStage::Compute};

    const std::filesystem::path bytecodePath =
        std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/shader_artifacts" /
        "compute.spirv.bin";
    const vector<byte> blob = ReadBinary(bytecodePath);
    ASSERT_FALSE(blob.empty());
    const auto artifact = shader::DecodeSpirvShaderArtifact(
        blob,
        shader::ShaderArtifactDecodeOptions{
            .Target = shader::ShaderTarget::SPIRV,
            .ExpectedGpuArtifact = test::ExpectedGpuArtifact(
                FindFixtureIndex("compute").value(), shader::ShaderTarget::SPIRV),
            .ExpectedToolchainIdentity = kFixtureToolchainIdentity},
        nullptr);
    ASSERT_TRUE(artifact.has_value());
    const auto bytecode = artifact->Generic().FindStageBytecode(shader::ShaderStage::Compute);
    ASSERT_TRUE(bytecode.has_value());
    Device* const device = Context.Device.get();

    auto layoutResult = VkDevice->CreatePipelineLayout(resolved.value());
    ASSERT_TRUE(layoutResult.HasValue());
    unique_ptr<PipelineLayout> layout = layoutResult.Release();
    // A second layout over the same resolved input: its handles look identical but carry another
    // generation, which is what makes them unusable here.
    auto otherResult = VkDevice->CreatePipelineLayout(resolved.value());
    ASSERT_TRUE(otherResult.HasValue());
    unique_ptr<PipelineLayout> otherLayout = otherResult.Release();

    const BindingHandle push = layout->FindBinding("TestConstants");
    const BindingHandle output = layout->FindBinding("Output");
    ASSERT_TRUE(push.IsValid());
    ASSERT_TRUE(output.IsValid());
    EXPECT_NE(push, output);
    const auto pushRecord = FindBackendBindingRecord(
        CastVkObject(layout.get())->_bindingNames,
        CastVkObject(layout.get())->_bindingGeneration,
        push);
    ASSERT_TRUE(pushRecord.HasValue());
    EXPECT_EQ(pushRecord.Get()->Kind, BackendBindingRecordKind::Push);
    const BindingHandle foreignPush = otherLayout->FindBinding("TestConstants");
    ASSERT_TRUE(foreignPush.IsValid());
    const BindingHandle collidingDescriptor = layout->FindBinding("UnusedAtPushLocation");
    ASSERT_TRUE(collidingDescriptor.IsValid());

    auto shaderResult = device->CreateShader(ShaderDescriptor{
        .Source = bytecode.value(),
        .Category = ShaderBlobCategory::SPIRV,
        .Stages = ShaderStage::Compute});
    ASSERT_TRUE(shaderResult.HasValue());
    unique_ptr<Shader> computeShader = shaderResult.Release();
    auto psoResult = device->CreateComputePipelineState(ComputePipelineStateDescriptor{
        .PipelineLayout = layout.get(),
        .CS = ShaderEntry{computeShader.get(), "CSMain"}});
    ASSERT_TRUE(psoResult.HasValue());
    unique_ptr<ComputePipelineState> pso = psoResult.Release();

    auto outputResult = device->CreateBuffer(BufferDescriptor{
        .Size = sizeof(uint32_t),
        .Memory = MemoryType::Device,
        .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource | BufferUse::CopyDestination,
        .Hints = ResourceHint::None});
    ASSERT_TRUE(outputResult.HasValue());
    unique_ptr<Buffer> outputBuffer = outputResult.Release();
    auto readbackResult = device->CreateBuffer(BufferDescriptor{
        .Size = sizeof(uint32_t),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::CopyDestination | BufferUse::MapRead,
        .Hints = ResourceHint::None});
    ASSERT_TRUE(readbackResult.HasValue());
    unique_ptr<Buffer> readback = readbackResult.Release();

    auto parameterSetResult = device->CreateShaderParameterSet(ShaderParameterSetDescriptor{
        .Layout = layout.get(),
        .GroupIndex = 0});
    ASSERT_TRUE(parameterSetResult.HasValue());
    unique_ptr<ShaderParameterSet> parameterSet = parameterSetResult.Release();
    const ShaderBufferBinding outputValue{
        .Target = outputBuffer.get(),
        .Range = BufferRange{0, sizeof(uint32_t)},
        .StructureByteStride = sizeof(uint32_t)};
    // A push handle names a push constant block, which owns no descriptor slot, so a parameter set
    // write through it has nowhere to land and must be refused instead of silently dropped.
    EXPECT_FALSE(parameterSet->Set(push, 0, outputValue));
    ASSERT_TRUE(parameterSet->Set(output, 0, outputValue));
    ASSERT_TRUE(parameterSet->FlushWrites());

    auto commandResult = device->CreateCommandBuffer(Context.Queue);
    ASSERT_TRUE(commandResult.HasValue());
    unique_ptr<CommandBuffer> command = commandResult.Release();
    command->Begin();
    const ResourceBarrierDescriptor toStorage = BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::Undefined,
        .After = BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&toStorage, 1});
    auto encoderResult = command->BeginComputePass();
    ASSERT_TRUE(encoderResult.HasValue());
    unique_ptr<ComputeCommandEncoder> encoder = encoderResult.Release();
    encoder->BindComputePipelineState(pso.get());

    const uint32_t constants[4]{1u, 2u, 3u, 4u};
    const std::span<const byte> constantBytes{
        reinterpret_cast<const byte*>(constants), sizeof(constants)};
    EXPECT_TRUE(encoder->SetPushConstants(push, constantBytes));
    // A descriptor handle names no push block, a partial write would leave the range half authored,
    // and a handle from another layout must not resolve here at all.
    EXPECT_FALSE(encoder->SetPushConstants(output, constantBytes));
    EXPECT_FALSE(encoder->SetPushConstants(collidingDescriptor, constantBytes));
    EXPECT_FALSE(encoder->SetPushConstants(push, constantBytes.subspan(0, 8)));
    EXPECT_FALSE(encoder->SetPushConstants(foreignPush, constantBytes));
    EXPECT_FALSE(encoder->SetPushConstants(BindingHandle{}, constantBytes));

    encoder->BindShaderParameterSet(0, parameterSet.get());
    encoder->Dispatch(1, 1, 1);
    command->EndComputePass(std::move(encoder));
    const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::UnorderedAccess,
        .After = BufferState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyBufferToBuffer(readback.get(), 0, outputBuffer.get(), 0, sizeof(uint32_t));
    command->End();
    CommandBuffer* commands[]{command.get()};
    Context.Queue->Submit(CommandQueueSubmitDescriptor{.CmdBuffers = commands});
    Context.Queue->Wait();

    // The dispatch still runs with the push range declared and written, so the extra range did not
    // invalidate the layout the shader was compiled against.
    void* mapped = readback->Map(0, sizeof(uint32_t));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange(BufferRange{0, sizeof(uint32_t)});
    const uint32_t value = *static_cast<const uint32_t*>(mapped);
    readback->Unmap();
    EXPECT_EQ(value, 0x12345678u);
}

}  // namespace radray::render

#endif
