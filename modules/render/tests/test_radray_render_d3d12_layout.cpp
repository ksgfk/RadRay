#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include <radray/render/shader_layout.h>
#include <radray/shader/shader_artifact.h>

#include "gpu_test_fixture.h"
#include "shader_contract_fixtures.h"

// M4 覆盖: D3D12 native chain 只从 ResolvedD3D12Layout 建立。这些测试要真设备,
// 因为 explicit carrier、static sampler 与 root descriptor 的 offset 语义只有在
// D3D12SerializeVersionedRootSignature / CreateRootSignature / 真 dispatch 之后才算成立。
#if defined(RADRAY_ENABLE_D3D12)

namespace radray::render {
namespace {

using d3d12::CastD3D12Object;
using d3d12::DeviceD3D12;
using d3d12::RootSigD3D12;
using d3d12::ShaderParameterSetLayoutEntryD3D12;

constexpr uint64_t kFixtureToolchainIdentity = 0x0000000001090212ull;
constexpr uint32_t kComputeWrittenValue = 0x12345678u;

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

// The decoded artifact is a view into the blob, so the two have to travel together. Moving the
// vector keeps its heap buffer, which is what keeps the view's pointers valid across the return.
struct FixtureArtifact {
    vector<byte> Blob;
    std::optional<shader::DxilShaderArtifactView> View;
};

FixtureArtifact DecodeFixture(std::string_view name) {
    const std::optional<size_t> index = FindFixtureIndex(name);
    if (!index.has_value()) {
        return {};
    }
    const std::filesystem::path path =
        std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/shader_artifacts" /
        (string{name} + ".dxil.bin");
    FixtureArtifact result{};
    result.Blob = ReadBinary(path);
    if (result.Blob.empty()) {
        return {};
    }
    result.View = shader::DecodeDxilShaderArtifact(
        result.Blob,
        shader::ShaderArtifactDecodeOptions{
            .Target = shader::ShaderTarget::DXIL,
            .ExpectedGpuArtifact = test::ExpectedGpuArtifact(index.value(), shader::ShaderTarget::DXIL),
            .ExpectedToolchainIdentity = kFixtureToolchainIdentity},
        nullptr);
    return result;
}

std::optional<ResolvedD3D12Layout> ResolveFixture(
    std::string_view name,
    const D3D12TargetLayoutOptions& options = {}) {
    const FixtureArtifact artifact = DecodeFixture(name);
    if (!artifact.View.has_value()) {
        return std::nullopt;
    }
    return ResolveD3D12Layout(artifact.View.value(), options);
}

// Hand-built layouts let a test describe shapes no fixture happens to produce: an illegal root
// descriptor placement, a static sampler with no carrier, or an authored carrier this repository has
// no HLSL for. The resolved layout is a plain owning value, so building one is the same thing
// resolution produces.
ResolvedD3D12Binding MakeBinding(
    std::string_view name,
    shader::ShaderBindingKind kind,
    uint32_t group,
    uint32_t binding,
    shader::ShaderBindingPlacement placement = shader::ShaderBindingPlacement::Table,
    ShaderStages stages = ShaderStages{ShaderStage::Compute}) {
    ResolvedD3D12Binding result{};
    result.Name = string{name};
    result.LogicalKind = kind;
    result.Group = group;
    result.Binding = binding;
    result.Count = 1;
    result.Stages = stages;
    result.Placement = placement;
    return result;
}

// Authors a root signature the way an HLSL `[RootSignature]` policy would: one root UAV at u2 in
// space 0, visible everywhere. There is no fixture with an authored root descriptor, and the parity
// this milestone promises is exactly between an authored root descriptor and one the implicit
// builder generated, so the carrier has to come from somewhere.
vector<byte> SerializeRootUavSignature() {
    D3D12_ROOT_PARAMETER1 parameter{};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameter.Descriptor.ShaderRegister = 2;
    parameter.Descriptor.RegisterSpace = 0;
    parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 1;
    desc.Desc_1_1.pParameters = &parameter;
    desc.Desc_1_1.NumStaticSamplers = 0;
    desc.Desc_1_1.pStaticSamplers = nullptr;
    desc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(::D3D12SerializeVersionedRootSignature(
            &desc,
            blob.GetAddressOf(),
            error.GetAddressOf()))) {
        return {};
    }
    const auto* bytes = static_cast<const byte*>(blob->GetBufferPointer());
    return vector<byte>{bytes, bytes + blob->GetBufferSize()};
}

struct D3D12DeviceFixture : ::testing::Test {
    test::DeviceContext Context;
    bool Available{false};
    DeviceD3D12* Device{nullptr};

    void SetUp() override {
        Available = test::TryCreateDevice(RenderBackend::D3D12, Context, true);
        if (Available) {
            // The resolved-layout overload is D3D12 specific: it is not on the shared Device
            // interface, because a Vulkan layout is a different resolved type.
            Device = static_cast<DeviceD3D12*>(Context.Device.get());
        }
    }
};

// Runs the `compute` fixture through a layout the caller supplies and returns what the shader wrote,
// read back from `readOffset`. The shader always writes element 0 of whatever address the root
// parameter or table entry points at, so the offset the dynamic offset applied is directly visible
// in which bytes changed.
std::optional<uint32_t> DispatchComputeAndReadBack(
    test::DeviceContext& context,
    const ResolvedD3D12Layout& resolvedLayout,
    uint32_t dynamicOffset,
    uint64_t bufferSize,
    uint64_t readOffset) {
    const FixtureArtifact artifact = DecodeFixture("compute");
    if (!artifact.View.has_value()) {
        return std::nullopt;
    }
    const auto bytecode = artifact.View->Generic().FindStageBytecode(shader::ShaderStage::Compute);
    if (!bytecode.has_value()) {
        return std::nullopt;
    }
    Device& device = *context.Device;
    auto* deviceD3D12 = static_cast<DeviceD3D12*>(context.Device.get());

    auto layoutResult = deviceD3D12->CreatePipelineLayout(resolvedLayout);
    if (!layoutResult.HasValue()) {
        return std::nullopt;
    }
    unique_ptr<PipelineLayout> layout = layoutResult.Release();
    const BindingHandle outputBinding = layout->FindBinding("Output");
    if (!outputBinding.IsValid()) {
        return std::nullopt;
    }

    auto shaderResult = device.CreateShader(ShaderDescriptor{
        .Source = bytecode.value(),
        .Category = ShaderBlobCategory::DXIL,
        .Stages = ShaderStage::Compute});
    if (!shaderResult.HasValue()) {
        return std::nullopt;
    }
    unique_ptr<Shader> computeShader = shaderResult.Release();
    auto psoResult = device.CreateComputePipelineState(ComputePipelineStateDescriptor{
        .PipelineLayout = layout.get(),
        .CS = ShaderEntry{computeShader.get(), "CSMain"}});
    if (!psoResult.HasValue()) {
        return std::nullopt;
    }
    unique_ptr<ComputePipelineState> pso = psoResult.Release();

    auto outputResult = device.CreateBuffer(BufferDescriptor{
        .Size = bufferSize,
        .Memory = MemoryType::Device,
        .Usage = BufferUse::UnorderedAccess | BufferUse::CopySource | BufferUse::CopyDestination,
        .Hints = ResourceHint::None});
    if (!outputResult.HasValue()) {
        return std::nullopt;
    }
    unique_ptr<Buffer> output = outputResult.Release();
    auto readbackResult = device.CreateBuffer(BufferDescriptor{
        .Size = sizeof(uint32_t),
        .Memory = MemoryType::ReadBack,
        .Usage = BufferUse::CopyDestination | BufferUse::MapRead,
        .Hints = ResourceHint::None});
    if (!readbackResult.HasValue()) {
        return std::nullopt;
    }
    unique_ptr<Buffer> readback = readbackResult.Release();

    auto parameterSetResult = device.CreateShaderParameterSet(ShaderParameterSetDescriptor{
        .Layout = layout.get(),
        .GroupIndex = 0});
    if (!parameterSetResult.HasValue()) {
        return std::nullopt;
    }
    unique_ptr<ShaderParameterSet> parameterSet = parameterSetResult.Release();
    if (!parameterSet->Set(
            outputBinding,
            0,
            ShaderBufferBinding{
                .Target = output.get(),
                .Range = BufferRange{0, sizeof(uint32_t)},
                .StructureByteStride = sizeof(uint32_t)})) {
        return std::nullopt;
    }
    if (!parameterSet->FlushWrites()) {
        return std::nullopt;
    }

    auto commandResult = device.CreateCommandBuffer(context.Queue);
    if (!commandResult.HasValue()) {
        return std::nullopt;
    }
    unique_ptr<CommandBuffer> command = commandResult.Release();
    command->Begin();
    const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{
        .Target = output.get(),
        .Before = BufferState::Undefined,
        .After = BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&toUav, 1});
    auto encoderResult = command->BeginComputePass();
    if (!encoderResult.HasValue()) {
        return std::nullopt;
    }
    unique_ptr<ComputeCommandEncoder> encoder = encoderResult.Release();
    encoder->BindComputePipelineState(pso.get());
    const ShaderParameterDynamicOffset offsets[]{
        ShaderParameterDynamicOffset{.Binding = outputBinding, .Offset = dynamicOffset}};
    if (resolvedLayout.Bindings[0].Placement == shader::ShaderBindingPlacement::RootDescriptor) {
        encoder->BindShaderParameterSet(0, parameterSet.get(), offsets);
    } else {
        encoder->BindShaderParameterSet(0, parameterSet.get());
    }
    encoder->Dispatch(1, 1, 1);
    command->EndComputePass(std::move(encoder));
    const ResourceBarrierDescriptor toCopy = BarrierBufferDescriptor{
        .Target = output.get(),
        .Before = BufferState::UnorderedAccess,
        .After = BufferState::CopySource};
    command->ResourceBarrier(std::span{&toCopy, 1});
    command->CopyBufferToBuffer(readback.get(), 0, output.get(), readOffset, sizeof(uint32_t));
    command->End();
    CommandBuffer* commands[]{command.get()};
    context.Queue->Submit(CommandQueueSubmitDescriptor{.CmdBuffers = commands});
    context.Queue->Wait();

    void* mapped = readback->Map(0, sizeof(uint32_t));
    if (mapped == nullptr) {
        return std::nullopt;
    }
    readback->InvalidateMappedRange(BufferRange{0, sizeof(uint32_t)});
    const uint32_t value = *static_cast<const uint32_t*>(mapped);
    readback->Unmap();
    return value;
}

}  // namespace

TEST_F(D3D12DeviceFixture, SamplerDescriptorsSupportMultipleViewFlights) {
    if (!Available) {
        GTEST_SKIP() << "no d3d12 device";
    }
    ResolvedD3D12Layout description;
    description.Bindings = {MakeBinding("ViewSamplers", shader::ShaderBindingKind::Sampler, 0, 0)};
    description.Bindings[0].Count = 2;
    auto created = Device->CreatePipelineLayout(description);
    ASSERT_TRUE(created.HasValue());
    auto layout = created.Release();
    // Several camera views and in-flight frames keep their immutable sets alive together.
    // Release the whole flight batch, then require that its descriptor ranges can be reused.
    for (uint32_t round = 0; round < 3; ++round) {
        vector<unique_ptr<ShaderParameterSet>> sets;
        for (uint32_t index = 0; index < 512; ++index) {
            auto set = Device->CreateShaderParameterSet({.Layout = layout.get(), .GroupIndex = 0});
            ASSERT_TRUE(set.HasValue()) << "round " << round << ", set " << index;
            sets.push_back(set.Release());
        }
    }
}

// A buffer placement modifier must move where the binding lives without changing what it is. The
// table lane and the root lane are read off the same fixture so the only difference is the modifier.
TEST_F(D3D12DeviceFixture, BufferPlacementDecidesTableVersusRootDescriptor) {
    if (!Available) {
        GTEST_SKIP() << "no d3d12 device";
    }
    const auto tableLayout = ResolveFixture("multiple_cbuffers");
    ASSERT_TRUE(tableLayout.has_value());
    ASSERT_EQ(tableLayout->Bindings.size(), 2u);
    EXPECT_FALSE(tableLayout->HasExplicitCarrier());

    auto tableResult = Device->CreatePipelineLayout(tableLayout.value());
    ASSERT_TRUE(tableResult.HasValue());
    unique_ptr<PipelineLayout> tableNative = tableResult.Release();
    RootSigD3D12* tableRootSig = CastD3D12Object(tableNative.get());
    ASSERT_EQ(tableRootSig->_parameterGroups.size(), 2u);
    for (const auto& group : tableRootSig->_parameterGroups) {
        EXPECT_TRUE(group.RootDescriptorOrder.empty());
        ASSERT_EQ(group.Entries.size(), 1u);
        EXPECT_EQ(group.Entries[0].LogicalKind, shader::ShaderBindingKind::CBuffer);
        EXPECT_EQ(group.Entries[0].Placement, shader::ShaderBindingPlacement::Table);
        // Namespace 0 is the CBV register class; it is what every binding handle lookup keys on
        // alongside the register number.
        EXPECT_EQ(group.Entries[0].Namespace, 0u);
        EXPECT_EQ(group.ResourceDescriptorCount, 1u);
        EXPECT_EQ(group.SamplerDescriptorCount, 0u);
    }
    EXPECT_TRUE(tableNative->FindBinding("First").IsValid());
    EXPECT_TRUE(tableNative->FindBinding("Second").IsValid());
    EXPECT_FALSE(tableNative->FindBinding("NotDeclared").IsValid());

    D3D12TargetLayoutOptions options{};
    options.BufferPlacements.push_back(D3D12BufferPlacementModifier{
        .Selector = ShaderLayoutSelector{
            .DeclarationName = "First",
            .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer},
        .Placement = D3D12BufferPlacement::RootDescriptor});
    const auto rootLayout = ResolveFixture("multiple_cbuffers", options);
    ASSERT_TRUE(rootLayout.has_value());
    EXPECT_NE(rootLayout->Hash, tableLayout->Hash);

    auto rootResult = Device->CreatePipelineLayout(rootLayout.value());
    ASSERT_TRUE(rootResult.HasValue());
    unique_ptr<PipelineLayout> rootNative = rootResult.Release();
    RootSigD3D12* rootRootSig = CastD3D12Object(rootNative.get());
    ASSERT_EQ(rootRootSig->_parameterGroups.size(), 2u);
    const auto& movedGroup = rootRootSig->_parameterGroups[0];
    ASSERT_EQ(movedGroup.RootDescriptorOrder.size(), 1u);
    EXPECT_EQ(movedGroup.RootDescriptorOrder[0], 0u);
    // Only the placement moved: a modifier that also changed the logical kind would let the layout
    // reclassify the resource behind the caller's back.
    EXPECT_EQ(movedGroup.Entries[0].LogicalKind, shader::ShaderBindingKind::CBuffer);
    EXPECT_EQ(movedGroup.Entries[0].Placement, shader::ShaderBindingPlacement::RootDescriptor);
    // A root descriptor owns no descriptor slot, so the group must stop reserving one.
    EXPECT_EQ(movedGroup.ResourceDescriptorCount, 0u);
    EXPECT_EQ(movedGroup.ResourceTableRootParameter, std::numeric_limits<uint32_t>::max());
    ASSERT_EQ(movedGroup.Bindings[0].RootDescriptorDestinations.size(), 1u);
    EXPECT_EQ(
        movedGroup.Bindings[0].RootDescriptorDestinations[0].Type,
        D3D12_ROOT_PARAMETER_TYPE_CBV);
    // The untouched group keeps its table.
    EXPECT_TRUE(rootRootSig->_parameterGroups[1].RootDescriptorOrder.empty());
    EXPECT_EQ(rootRootSig->_parameterGroups[1].ResourceDescriptorCount, 1u);
}

// A policy static sampler exists only inside the carrier. It must reserve no descriptor slot and it
// must not be writable through a parameter set, because a write there would be silently discarded.
TEST_F(D3D12DeviceFixture, PolicyStaticSamplerComesFromTheCarrierAndCannotBeWritten) {
    if (!Available) {
        GTEST_SKIP() << "no d3d12 device";
    }
    const auto layout = ResolveFixture("shadow_static_sampler");
    ASSERT_TRUE(layout.has_value());
    EXPECT_TRUE(layout->HasExplicitCarrier());

    auto nativeResult = Device->CreatePipelineLayout(layout.value());
    ASSERT_TRUE(nativeResult.HasValue());
    unique_ptr<PipelineLayout> native = nativeResult.Release();
    RootSigD3D12* rootSig = CastD3D12Object(native.get());
    ASSERT_EQ(rootSig->_staticSamplers.size(), 1u);
    ASSERT_EQ(rootSig->_parameterGroups.size(), 1u);
    const auto& group = rootSig->_parameterGroups[0];
    ASSERT_EQ(group.Entries.size(), 2u);
    const ShaderParameterSetLayoutEntryD3D12* samplerEntry = nullptr;
    const ShaderParameterSetLayoutEntryD3D12* textureEntry = nullptr;
    for (const auto& entry : group.Entries) {
        if (entry.IsSampler()) {
            samplerEntry = &entry;
        } else {
            textureEntry = &entry;
        }
    }
    ASSERT_NE(samplerEntry, nullptr);
    ASSERT_NE(textureEntry, nullptr);
    EXPECT_TRUE(samplerEntry->IsStaticSampler());
    EXPECT_EQ(textureEntry->Placement, shader::ShaderBindingPlacement::Table);
    // The carrier declares the sampler itself, so no sampler descriptor may be reserved for it.
    EXPECT_EQ(group.SamplerDescriptorCount, 0u);
    EXPECT_EQ(group.ResourceDescriptorCount, 1u);

    const BindingHandle samplerHandle = native->FindBinding("ShadowSampler");
    const BindingHandle textureHandle = native->FindBinding("ShadowTexture");
    ASSERT_TRUE(samplerHandle.IsValid());
    ASSERT_TRUE(textureHandle.IsValid());

    auto setResult = Device->CreateShaderParameterSet(ShaderParameterSetDescriptor{
        .Layout = native.get(),
        .GroupIndex = 0});
    ASSERT_TRUE(setResult.HasValue());
    unique_ptr<ShaderParameterSet> parameterSet = setResult.Release();
    auto samplerResult = Device->CreateSampler(SamplerDescriptor{});
    ASSERT_TRUE(samplerResult.HasValue());
    unique_ptr<Sampler> sampler = samplerResult.Release();
    EXPECT_FALSE(parameterSet->Set(samplerHandle, 0, sampler.get()));

    auto textureResult = Context.Device->CreateTexture(TextureDescriptor{
        .Dim = TextureDimension::Dim2D,
        .Width = 4,
        .Height = 4,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = TextureFormat::RGBA8_UNORM,
        .Memory = MemoryType::Device,
        .Usage = TextureUse::Resource,
        .Hints = ResourceHint::None});
    ASSERT_TRUE(textureResult.HasValue());
    unique_ptr<Texture> texture = textureResult.Release();
    auto viewResult = Context.Device->CreateTextureView(TextureViewDescriptor{
        .Target = texture.get(),
        .Dim = TextureDimension::Dim2D,
        .Format = TextureFormat::RGBA8_UNORM,
        .Range = SubresourceRange{0, 1, 0, 1},
        .Usage = TextureViewUsage::Resource});
    ASSERT_TRUE(viewResult.HasValue());
    unique_ptr<TextureView> view = viewResult.Release();
    EXPECT_TRUE(parameterSet->Set(textureHandle, 0, view.get()));
    EXPECT_TRUE(parameterSet->FlushWrites());
}

// Every root constant the carrier declares has to reach the push handle table, and a carrier that
// does not cover one has to fail closed rather than leave the handle silently unbound.
TEST_F(D3D12DeviceFixture, EveryAuthoredRootConstantReachesThePushTable) {
    if (!Available) {
        GTEST_SKIP() << "no d3d12 device";
    }
    const auto layout = ResolveFixture("multiple_root_constants");
    ASSERT_TRUE(layout.has_value());
    EXPECT_TRUE(layout->HasExplicitCarrier());
    ASSERT_EQ(layout->PushConstants.size(), 2u);

    auto nativeResult = Device->CreatePipelineLayout(layout.value());
    ASSERT_TRUE(nativeResult.HasValue());
    unique_ptr<PipelineLayout> native = nativeResult.Release();
    RootSigD3D12* rootSig = CastD3D12Object(native.get());
    ASSERT_EQ(rootSig->_pushConstantBindings.size(), 2u);
    // Push blocks are reachable by declaration name through the same handle table as the descriptor
    // bindings. A handle is opaque to callers, so what is pinned here is the record it names: the
    // push kind, and the register the carrier authored.
    const BindingHandle objectHandle = native->FindBinding("ObjectConstants");
    const BindingHandle materialHandle = native->FindBinding("MaterialConstants");
    ASSERT_TRUE(objectHandle.IsValid());
    ASSERT_TRUE(materialHandle.IsValid());
    EXPECT_NE(objectHandle, materialHandle);
    const auto objectRecord = FindBackendBindingRecord(
        rootSig->_bindingNames, rootSig->_bindingGeneration, objectHandle);
    const auto materialRecord = FindBackendBindingRecord(
        rootSig->_bindingNames, rootSig->_bindingGeneration, materialHandle);
    ASSERT_TRUE(objectRecord.HasValue());
    ASSERT_TRUE(materialRecord.HasValue());
    EXPECT_EQ(objectRecord.Get()->Kind, BackendBindingRecordKind::Push);
    EXPECT_EQ(materialRecord.Get()->Kind, BackendBindingRecordKind::Push);
    EXPECT_EQ(objectRecord.Get()->Location.Binding, 0u);
    EXPECT_EQ(materialRecord.Get()->Location.Binding, 1u);
    EXPECT_FALSE(native->FindBinding("NotDeclared").IsValid());

    // Drop one block from the resolved layout and the carrier now declares a root parameter nothing
    // claims, which is allowed; drop the carrier's coverage instead and creation must fail. The
    // reachable half of that pair is a resolved block the carrier does not declare.
    ResolvedD3D12Layout uncovered = layout.value();
    uncovered.PushConstants.push_back(ResolvedPushConstantBlock{
        .Name = "NotInTheCarrier",
        .RegisterSpace = 7,
        .Register = 7,
        .Offset = 0,
        .Size = 16,
        .Stages = ShaderStages{ShaderStage::Vertex}});
    EXPECT_FALSE(Device->CreatePipelineLayout(uncovered).HasValue());
}

// An illegal placement has to fail native creation rather than fall back to a nearby topology.
TEST_F(D3D12DeviceFixture, IllegalPlacementFailsNativeCreation) {
    if (!Available) {
        GTEST_SKIP() << "no d3d12 device";
    }
    // A texture cannot be a root descriptor: D3D12 root CBV/SRV/UAV only take buffer addresses.
    ResolvedD3D12Layout rootTexture{};
    rootTexture.Bindings = {MakeBinding(
        "Albedo",
        shader::ShaderBindingKind::Texture,
        0,
        0,
        shader::ShaderBindingPlacement::RootDescriptor)};
    EXPECT_FALSE(Device->CreatePipelineLayout(rootTexture).HasValue());

    // A root descriptor addresses exactly one resource, so an array cannot be one.
    ResolvedD3D12Layout rootArray{};
    rootArray.Bindings = {MakeBinding(
        "Items",
        shader::ShaderBindingKind::StructuredBuffer,
        0,
        0,
        shader::ShaderBindingPlacement::RootDescriptor)};
    rootArray.Bindings[0].Count = 4;
    EXPECT_FALSE(Device->CreatePipelineLayout(rootArray).HasValue());

    // A static sampler is authored by a carrier. Without one there is nothing to describe it, so the
    // implicit builder must refuse instead of quietly dropping the binding.
    ResolvedD3D12Layout carrierlessStaticSampler{};
    carrierlessStaticSampler.Bindings = {MakeBinding(
        "Fixed",
        shader::ShaderBindingKind::Sampler,
        0,
        0,
        shader::ShaderBindingPlacement::StaticSampler)};
    EXPECT_FALSE(Device->CreatePipelineLayout(carrierlessStaticSampler).HasValue());

    // Unordered bindings mean the resolver's own invariant broke; the backend must not paper over it
    // by re-sorting, because then a resolver bug would never surface.
    ResolvedD3D12Layout unordered{};
    unordered.Bindings = {
        MakeBinding("Second", shader::ShaderBindingKind::CBuffer, 0, 1),
        MakeBinding("First", shader::ShaderBindingKind::CBuffer, 0, 0)};
    EXPECT_FALSE(Device->CreatePipelineLayout(unordered).HasValue());
}

// The checkpoint this milestone is built around: the same arena offset has to land on the same bytes
// whether the root descriptor came from a placement modifier or from an authored carrier. Both lanes
// run the same DXIL against the same buffer, so a difference could only come from the layout.
TEST_F(D3D12DeviceFixture, SameOffsetLandsOnTheSameBytesThroughEitherTopology) {
    if (!Available) {
        GTEST_SKIP() << "no d3d12 device";
    }
    constexpr uint32_t kOffset = 256;
    constexpr uint64_t kBufferSize = 1024;

    D3D12TargetLayoutOptions options{};
    options.BufferPlacements.push_back(D3D12BufferPlacementModifier{
        .Selector = ShaderLayoutSelector{
            .DeclarationName = "Output",
            .ExpectedLogicalResourceKind = shader::ShaderBindingKind::RWStructuredBuffer},
        .Placement = D3D12BufferPlacement::RootDescriptor});
    const auto implicitLayout = ResolveFixture("compute", options);
    ASSERT_TRUE(implicitLayout.has_value());
    ASSERT_EQ(implicitLayout->Bindings.size(), 1u);
    EXPECT_FALSE(implicitLayout->HasExplicitCarrier());
    EXPECT_EQ(
        implicitLayout->Bindings[0].Placement,
        shader::ShaderBindingPlacement::RootDescriptor);

    const auto implicitValue =
        DispatchComputeAndReadBack(Context, implicitLayout.value(), kOffset, kBufferSize, kOffset);
    ASSERT_TRUE(implicitValue.has_value());
    EXPECT_EQ(implicitValue.value(), kComputeWrittenValue);
    // Nothing may have been written at offset 0: that would mean the dynamic offset was dropped.
    const auto implicitUntouched =
        DispatchComputeAndReadBack(Context, implicitLayout.value(), kOffset, kBufferSize, 0);
    ASSERT_TRUE(implicitUntouched.has_value());
    EXPECT_EQ(implicitUntouched.value(), 0u);

    ResolvedD3D12Layout explicitLayout = implicitLayout.value();
    explicitLayout.SerializedRootSignature = SerializeRootUavSignature();
    ASSERT_FALSE(explicitLayout.SerializedRootSignature.empty());
    ASSERT_TRUE(explicitLayout.HasExplicitCarrier());

    const auto explicitValue =
        DispatchComputeAndReadBack(Context, explicitLayout, kOffset, kBufferSize, kOffset);
    ASSERT_TRUE(explicitValue.has_value());
    EXPECT_EQ(explicitValue.value(), implicitValue.value());
    const auto explicitUntouched =
        DispatchComputeAndReadBack(Context, explicitLayout, kOffset, kBufferSize, 0);
    ASSERT_TRUE(explicitUntouched.has_value());
    EXPECT_EQ(explicitUntouched.value(), 0u);
}

// Root constants go through the same handle table as descriptor bindings, so this drives the whole
// path: declaration name -> handle -> root parameter -> real dispatch. The `compute` fixture declares
// no root constants, so the block is added to the resolved layout the way a pipeline recipe would ask
// for one; a D3D12 root signature is allowed to declare more than the shader actually reads.
TEST_F(D3D12DeviceFixture, PushHandleWritesRootConstantsAndRejectsMisuse) {
    if (!Available) {
        GTEST_SKIP() << "no d3d12 device";
    }
    auto resolved = ResolveFixture("compute");
    ASSERT_TRUE(resolved.has_value());
    ASSERT_EQ(resolved->Bindings.size(), 1u);
    EXPECT_FALSE(resolved->HasExplicitCarrier());
    resolved->PushConstants.push_back(ResolvedPushConstantBlock{
        .Name = "TestConstants",
        .RegisterSpace = 0,
        .Register = 0,
        .Offset = 0,
        .Size = 16,
        .Stages = ShaderStage::Compute});

    const FixtureArtifact artifact = DecodeFixture("compute");
    ASSERT_TRUE(artifact.View.has_value());
    const auto bytecode = artifact.View->Generic().FindStageBytecode(shader::ShaderStage::Compute);
    ASSERT_TRUE(bytecode.has_value());
    // `Device` is the fixture's DeviceD3D12*, so the shared interface comes from the context.
    ::radray::render::Device* const device = Context.Device.get();

    auto layoutResult = Device->CreatePipelineLayout(resolved.value());
    ASSERT_TRUE(layoutResult.HasValue());
    unique_ptr<PipelineLayout> layout = layoutResult.Release();
    // A second layout over the same resolved input: its handles look identical but carry another
    // generation, which is what makes them unusable here.
    auto otherResult = Device->CreatePipelineLayout(resolved.value());
    ASSERT_TRUE(otherResult.HasValue());
    unique_ptr<PipelineLayout> otherLayout = otherResult.Release();

    const BindingHandle push = layout->FindBinding("TestConstants");
    const BindingHandle output = layout->FindBinding("Output");
    ASSERT_TRUE(push.IsValid());
    ASSERT_TRUE(output.IsValid());
    EXPECT_NE(push, output);
    const BindingHandle foreignPush = otherLayout->FindBinding("TestConstants");
    ASSERT_TRUE(foreignPush.IsValid());

    auto shaderResult = device->CreateShader(ShaderDescriptor{
        .Source = bytecode.value(),
        .Category = ShaderBlobCategory::DXIL,
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
    // A push handle names a root constant block, which owns no descriptor slot, so a parameter set
    // write through it has nowhere to land and must be refused instead of silently dropped.
    EXPECT_FALSE(parameterSet->Set(push, 0, outputValue));
    ASSERT_TRUE(parameterSet->Set(output, 0, outputValue));
    ASSERT_TRUE(parameterSet->FlushWrites());

    auto commandResult = device->CreateCommandBuffer(Context.Queue);
    ASSERT_TRUE(commandResult.HasValue());
    unique_ptr<CommandBuffer> command = commandResult.Release();
    command->Begin();
    const ResourceBarrierDescriptor toUav = BarrierBufferDescriptor{
        .Target = outputBuffer.get(),
        .Before = BufferState::Undefined,
        .After = BufferState::UnorderedAccess};
    command->ResourceBarrier(std::span{&toUav, 1});
    auto encoderResult = command->BeginComputePass();
    ASSERT_TRUE(encoderResult.HasValue());
    unique_ptr<ComputeCommandEncoder> encoder = encoderResult.Release();
    encoder->BindComputePipelineState(pso.get());

    const uint32_t constants[4]{1u, 2u, 3u, 4u};
    const std::span<const byte> constantBytes{
        reinterpret_cast<const byte*>(constants), sizeof(constants)};
    EXPECT_TRUE(encoder->SetPushConstants(push, constantBytes));
    // A descriptor handle names no push block, a partial write would leave the block half authored,
    // and a handle from another layout must not resolve here at all.
    EXPECT_FALSE(encoder->SetPushConstants(output, constantBytes));
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

    // The dispatch still runs with the root constants written, so the extra root parameter did not
    // break the layout the shader was compiled against.
    void* mapped = readback->Map(0, sizeof(uint32_t));
    ASSERT_NE(mapped, nullptr);
    readback->InvalidateMappedRange(BufferRange{0, sizeof(uint32_t)});
    EXPECT_EQ(*static_cast<const uint32_t*>(mapped), kComputeWrittenValue);
    readback->Unmap();
}

}  // namespace radray::render

#endif
