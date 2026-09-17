#include <gtest/gtest.h>

#include <type_traits>

#include <filesystem>
#include <fstream>
#include <limits>

#include <radray/render/shader_layout.h>
#include <radray/shader/shader_artifact.h>

#include "gpu_test_fixture.h"
#include "shader_contract_fixtures.h"

// M3 覆盖: Vulkan native chain 只从 ResolvedVulkanLayout 建立。这些测试要真设备,
// 因为 immutable sampler 与 empty set hole 只有在 vkCreateDescriptorSetLayout /
// vkCreatePipelineLayout 真正接受之后才算成立。
#if defined(RADRAY_ENABLE_VULKAN)
#include "vk/pipeline_layout_cache_vulkan.h"

static_assert(std::has_virtual_destructor_v<radray::render::vulkan::PipelineLayoutCacheVulkan>);
static_assert(std::has_virtual_destructor_v<radray::render::vulkan::CachedPipelineLayoutVulkan>);

namespace radray::render {
namespace {

using vulkan::CastVkObject;
using vulkan::DeviceVulkan;
using vulkan::ShaderParameterSetLayoutEntryVulkan;

constexpr uint64_t kFixtureToolchainIdentity = shader::kShaderToolchainIdentity;

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

TEST_F(VulkanDeviceFixture, DescriptorWritesRejectInvalidObjectsWithoutCachingValues) {
    if (!Available) GTEST_SKIP() << "Vulkan is unavailable";
    ResolvedVulkanLayout description;
    description.SetCount = 1;
    description.Bindings = {
        MakeBinding("Buffer", shader::ShaderBindingKind::CBuffer, 0, 0),
        MakeBinding("Typed", shader::ShaderBindingKind::TypedBuffer, 0, 1),
        MakeBinding("Texture", shader::ShaderBindingKind::Texture, 0, 2),
        MakeBinding("Sampler", shader::ShaderBindingKind::Sampler, 0, 3)};
    auto layout = VkDevice->CreatePipelineLayout(description);
    ASSERT_TRUE(layout.HasValue());
    EXPECT_FALSE(VkDevice->CreateShaderParameterSet({.Layout = layout.Get(), .GroupIndex = 1}).HasValue());
    auto set = VkDevice->CreateShaderParameterSet({.Layout = layout.Get(), .GroupIndex = 0});
    ASSERT_TRUE(set.HasValue());
    auto buffer = VkDevice->CreateBuffer({256, MemoryType::Upload, BufferUse::CBuffer | BufferUse::Resource});
    auto sampler = VkDevice->CreateSampler({});
    ASSERT_TRUE(buffer.HasValue());
    ASSERT_TRUE(sampler.HasValue());
    const auto bufferHandle = layout->FindBinding("Buffer");
    const auto typedHandle = layout->FindBinding("Typed");
    const auto samplerHandle = layout->FindBinding("Sampler");
    const auto textureHandle = layout->FindBinding("Texture");
    EXPECT_FALSE(set->Set(bufferHandle, 0, sampler.Get()));
    EXPECT_FALSE(set->Set(bufferHandle, 0, ShaderBufferBinding{nullptr, {0, 256}, 0}));
    EXPECT_FALSE(set->Set(typedHandle, 0, ShaderBufferBinding{buffer.Get(), {0, 256}, 0}));
    EXPECT_FALSE(set->Set(typedHandle, 0, ShaderTexelBufferBinding{nullptr, {0, 4}, TextureFormat::R32_UINT}));
    EXPECT_FALSE(set->Set(textureHandle, 0, sampler.Get()));
    EXPECT_FALSE(set->Set(textureHandle, 0, static_cast<TextureView*>(nullptr)));
    EXPECT_FALSE(set->Set(samplerHandle, 0, ShaderBufferBinding{buffer.Get(), {0, 256}, 0}));
    EXPECT_FALSE(set->Set(samplerHandle, 0, static_cast<Sampler*>(nullptr)));
    ASSERT_TRUE(set->Set(bufferHandle, 0, ShaderBufferBinding{buffer.Get(), {0, 256}, 0}));
    ASSERT_TRUE(set->Set(samplerHandle, 0, sampler.Get()));
    const VulkanCommandQueueDescriptor foreignQueues[]{{QueueType::Direct, 1}};
    VulkanDeviceDescriptor foreignDescription{};
    foreignDescription.Queues = foreignQueues;
    auto foreign = Device::Create(DeviceDescriptor{foreignDescription});
    ASSERT_TRUE(foreign.HasValue());
    auto foreignBuffer = foreign->CreateBuffer({256, MemoryType::Upload, BufferUse::CBuffer});
    auto foreignSampler = foreign->CreateSampler({});
    ASSERT_TRUE(foreignBuffer.HasValue());
    ASSERT_TRUE(foreignSampler.HasValue());
    EXPECT_FALSE(set->Set(bufferHandle, 0, ShaderBufferBinding{foreignBuffer.Get(), {0, 256}, 0}));
    EXPECT_FALSE(set->Set(samplerHandle, 0, foreignSampler.Get()));
    buffer->Destroy();
    sampler->Destroy();
    EXPECT_FALSE(set->Set(bufferHandle, 0, ShaderBufferBinding{buffer.Get(), {0, 256}, 0}));
    EXPECT_FALSE(set->Set(samplerHandle, 0, sampler.Get()));
    layout->Destroy();
    EXPECT_FALSE(set->Set(bufferHandle, 0, ShaderBufferBinding{buffer.Get(), {0, 256}, 0}));
    EXPECT_FALSE(set->FlushWrites());
    EXPECT_FALSE(VkDevice->CreateShaderParameterSet({.Layout = layout.Get(), .GroupIndex = 0}).HasValue());
    EXPECT_EQ(Context.ValidationErrors.load(), 0u);
}

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
    ASSERT_EQ(native->GetSetLayouts().size(), 3u);
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
    ASSERT_EQ(native->GetSetLayouts().size(), 5u);
    for (uint32_t setIndex = 0; setIndex < 4; ++setIndex) {
        EXPECT_TRUE(native->_parameterSetLayouts[setIndex].empty()) << setIndex;
    }
    ASSERT_EQ(VkDevice->_samplerCache.size(), 1u);
    EXPECT_NE(VkDevice->_samplerCache.begin()->second->_sampler, VK_NULL_HANDLE);

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

TEST_F(VulkanDeviceFixture, OrdinaryAndImmutableSamplersShareDeviceCacheInEitherCreationOrder) {
    if (!Available) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    for (uint32_t immutableFirst = 0; immutableFirst < 2; ++immutableFirst) {
        SamplerDescriptor desc{};
        desc.AddressS = desc.AddressT = desc.AddressR = AddressMode::Repeat;
        desc.LodMax = static_cast<float>(immutableFirst);
        VulkanImmutableSamplerState state{};
        state.MaxLod = desc.LodMax;
        ResolvedVulkanLayout resolved{};
        resolved.SetCount = 1;
        resolved.Bindings = {MakeBinding("CachedSampler", shader::ShaderBindingKind::Sampler, 0, 0)};
        resolved.Bindings[0].ImmutableSamplerIndex = 0;
        resolved.ImmutableSamplers = {state};

        Nullable<Sampler*> ordinary = nullptr;
        if (!immutableFirst) {
            ordinary = VkDevice->GetOrCreateSampler(desc);
            ASSERT_TRUE(ordinary.HasValue());
        }
        auto first = VkDevice->CreatePipelineLayout(resolved);
        ASSERT_TRUE(first.HasValue());
        auto second = VkDevice->CreatePipelineLayout(resolved);
        ASSERT_TRUE(second.HasValue());
        if (immutableFirst) {
            ordinary = VkDevice->GetOrCreateSampler(desc);
            ASSERT_TRUE(ordinary.HasValue());
        }
        const auto cached = VkDevice->_samplerCache.find(state);
        ASSERT_NE(cached, VkDevice->_samplerCache.end());
        EXPECT_EQ(cached->second.get(), CastVkObject(ordinary.Get()));
        EXPECT_EQ(VkDevice->_samplerCache.size(), immutableFirst + 1u);
        EXPECT_EQ(VkDevice->GetOrCreateSampler(desc).Get(), ordinary.Get());
        auto owned = VkDevice->CreateSampler(desc);
        ASSERT_TRUE(owned.HasValue());
        EXPECT_NE(owned.Get(), ordinary.Get());
        EXPECT_EQ(VkDevice->_samplerCache.size(), immutableFirst + 1u);
        owned.Get()->Destroy();
        auto retainedSetLayout = CastVkObject(first.Get())->GetSetLayouts()[0];
        EXPECT_EQ(retainedSetLayout.Get(), CastVkObject(second.Get())->GetSetLayouts()[0].Get());

        auto parameterSet = VkDevice->CreateShaderParameterSet(
            ShaderParameterSetDescriptor{.Layout = second.Get(), .GroupIndex = 0});
        ASSERT_TRUE(parameterSet.HasValue());
        first.Get()->Destroy();
        EXPECT_TRUE(parameterSet.Get()->FlushWrites());
        parameterSet.Get()->Destroy();
        second.Get()->Destroy();
        EXPECT_TRUE(ordinary.Get()->IsValid());

        // A retained native set layout must still carry a live sampler after its pipelines are gone.
        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1};
        const auto allocation = VkDevice->_descriptorSetAllocator.Allocate({.Layout = retainedSetLayout->Get(),
                                                                            .DescriptorCounts = std::span{&poolSize, 1}});
        ASSERT_TRUE(allocation.has_value());
        VkDevice->_descriptorSetAllocator.Destroy(allocation.value());
    }
    Context.Reset();
    EXPECT_EQ(Context.ValidationErrors.load(), 0u);
}

TEST_F(VulkanDeviceFixture, FullSamplerStateStaysDistinctAndSignedZeroReusesCache) {
    if (!Available) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    {
        VulkanImmutableSamplerState base{};
        base.AddressModeU = base.AddressModeV = base.AddressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        vector<VulkanImmutableSamplerState> states{base};
        auto changed = base;
        changed.MipLodBias = 0.25f;
        states.push_back(changed);
        changed = base;
        changed.BorderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        states.push_back(changed);
        changed = base;
        changed.Flags = shader::kShaderSamplerFlagUnnormalizedCoordinates;
        states.push_back(changed);
        if (VkDevice->_extFeatures.feature12.samplerFilterMinmax == VK_TRUE) {
            changed = base;
            changed.ReductionMode = VK_SAMPLER_REDUCTION_MODE_MIN;
            states.push_back(changed);
            changed.ReductionMode = VK_SAMPLER_REDUCTION_MODE_MAX;
            states.push_back(changed);
        }
        vector<unique_ptr<PipelineLayout>> layouts;
        for (const auto& state : states) {
            ResolvedVulkanLayout resolved{};
            resolved.SetCount = 1;
            resolved.Bindings = {MakeBinding("Immutable", shader::ShaderBindingKind::Sampler, 0, 0)};
            resolved.Bindings[0].ImmutableSamplerIndex = 0;
            resolved.ImmutableSamplers = {state};
            auto layout = VkDevice->CreatePipelineLayout(resolved);
            ASSERT_TRUE(layout.HasValue());
            layouts.push_back(layout.Release());
            EXPECT_EQ(VkDevice->_samplerCache.size(), layouts.size());
        }
        auto negativeZero = base;
        negativeZero.MipLodBias = -0.0f;
        negativeZero.MinLod = -0.0f;
        negativeZero.MaxLod = -0.0f;
        const auto same = VkDevice->GetOrCreateSamplerInternal(negativeZero);
        ASSERT_TRUE(same.HasValue());
        EXPECT_EQ(same.Get(), VkDevice->_samplerCache.find(base)->second.get());
        EXPECT_EQ(VkDevice->_samplerCache.size(), states.size());
    }
    Context.Reset();
    EXPECT_EQ(Context.ValidationErrors.load(), 0u);
}

TEST_F(VulkanDeviceFixture, RejectedSamplerDoesNotPopulateDeviceCache) {
    if (!Available) {
        GTEST_SKIP() << "Vulkan is unavailable on this machine";
    }
    VulkanImmutableSamplerState state{};
    state.MipLodBias = VkDevice->_properties.limits.maxSamplerLodBias + 1.0f;
    EXPECT_FALSE(VkDevice->GetOrCreateSamplerInternal(state).HasValue());
    EXPECT_TRUE(VkDevice->_samplerCache.empty());
    state.MipLodBias = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(VkDevice->GetOrCreateSamplerInternal(state).HasValue());
    EXPECT_TRUE(VkDevice->_samplerCache.empty());
    state.MipLodBias = 0.0f;
    EXPECT_TRUE(VkDevice->GetOrCreateSamplerInternal(state).HasValue());
    EXPECT_EQ(VkDevice->_samplerCache.size(), 1u);
    Context.Reset();
    EXPECT_EQ(Context.ValidationErrors.load(), 0u);
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

TEST_F(VulkanDeviceFixture, NativeLayoutCacheSharesRenamedWrappersAndEvictsLastUser) {
    if (!Available) GTEST_SKIP() << "no vulkan device";
    ResolvedVulkanLayout desc;
    desc.SetCount = 1;
    desc.Bindings = {MakeBinding("First", shader::ShaderBindingKind::CBuffer, 0, 0)};
    auto firstResult = VkDevice->CreatePipelineLayout(desc);
    ASSERT_TRUE(firstResult);
    auto first = firstResult.Release();
    desc.Bindings[0].Name = "Second";
    auto secondResult = VkDevice->CreatePipelineLayout(desc);
    ASSERT_TRUE(secondResult);
    auto second = secondResult.Release();
    EXPECT_NE(first.get(), second.get());
    EXPECT_EQ(CastVkObject(first.get())->GetNative(), CastVkObject(second.get())->GetNative());
    EXPECT_EQ(VkDevice->_pipelineLayoutCache->GetEntryCount(), 1u);
    auto set = VkDevice->CreateShaderParameterSet({.Layout = second.get(), .GroupIndex = 0});
    ASSERT_TRUE(set);
    auto buffer = VkDevice->CreateBuffer({.Size = 256, .Memory = MemoryType::Upload, .Usage = BufferUse::CBuffer});
    ASSERT_TRUE(buffer);
    const ShaderBufferBinding value{.Target = buffer.Get(), .Range = {0, 256}};
    EXPECT_FALSE(set.Get()->Set(first->FindBinding("First"), 0, value));
    EXPECT_TRUE(set.Get()->Set(second->FindBinding("Second"), 0, value));
    EXPECT_FALSE(second->FindBinding("First").IsValid());
    first->Destroy();
    first->Destroy();
    first.reset();
    EXPECT_TRUE(second->IsValid());
    EXPECT_TRUE(set.Get()->FlushWrites());
    EXPECT_EQ(VkDevice->_pipelineLayoutCache->GetEntryCount(), 1u);
    set = nullptr;
    second.reset();
    EXPECT_EQ(VkDevice->_pipelineLayoutCache->GetEntryCount(), 0u);
    EXPECT_EQ(VkDevice->_descriptorSetLayoutCache.GetLayoutCount(), 0u);
    auto recreated = VkDevice->CreatePipelineLayout(desc);
    ASSERT_TRUE(recreated);
}

TEST_F(VulkanDeviceFixture, NativeLayoutCacheKeysNativeFactsAndSharesSetLayouts) {
    if (!Available) GTEST_SKIP() << "no vulkan device";
    auto desc = MakeMixedLayout();
    auto first = VkDevice->CreatePipelineLayout(desc);
    ASSERT_TRUE(first);
    auto same = VkDevice->CreatePipelineLayout(desc);
    ASSERT_TRUE(same);
    EXPECT_EQ(CastVkObject(first.Get())->GetNative(), CastVkObject(same.Get())->GetNative());
    auto withPush = desc;
    withPush.PushBlock = ResolvedPushConstantBlock{.Name = "Push", .Size = 16, .Stages = ShaderStage::Vertex};
    auto pushed = VkDevice->CreatePipelineLayout(withPush);
    ASSERT_TRUE(pushed);
    EXPECT_NE(CastVkObject(first.Get())->GetNative(), CastVkObject(pushed.Get())->GetNative());
    EXPECT_EQ(CastVkObject(first.Get())->GetSetLayouts()[0], CastVkObject(pushed.Get())->GetSetLayouts()[0]);
    withPush.PushBlock->Size = 32;
    auto biggerPush = VkDevice->CreatePipelineLayout(withPush);
    ASSERT_TRUE(biggerPush);
    EXPECT_NE(CastVkObject(pushed.Get())->GetNative(), CastVkObject(biggerPush.Get())->GetNative());
    withPush.PushBlock->Stages = ShaderStage::Pixel;
    auto otherPushStage = VkDevice->CreatePipelineLayout(withPush);
    ASSERT_TRUE(otherPushStage);
    EXPECT_NE(CastVkObject(biggerPush.Get())->GetNative(), CastVkObject(otherPushStage.Get())->GetNative());
    auto changed = desc;
    changed.Bindings[0].Stages = ShaderStage::Vertex;
    auto stage = VkDevice->CreatePipelineLayout(changed);
    ASSERT_TRUE(stage);
    EXPECT_NE(CastVkObject(first.Get())->GetNative(), CastVkObject(stage.Get())->GetNative());
    changed = desc;
    changed.Bindings[0].Placement = VulkanBufferDescriptorPlacement::Regular;
    changed.DynamicOffsetOrder = {1};
    auto regular = VkDevice->CreatePipelineLayout(changed);
    ASSERT_TRUE(regular);
    EXPECT_NE(CastVkObject(first.Get())->GetNative(), CastVkObject(regular.Get())->GetNative());
    changed = desc;
    ++changed.SetCount;
    auto extraHole = VkDevice->CreatePipelineLayout(changed);
    ASSERT_TRUE(extraHole);
    EXPECT_NE(CastVkObject(first.Get())->GetNative(), CastVkObject(extraHole.Get())->GetNative());
    const auto count = VkDevice->_pipelineLayoutCache->GetEntryCount();
    changed = desc;
    changed.DynamicOffsetOrder = {0};
    EXPECT_FALSE(VkDevice->CreatePipelineLayout(changed));
    EXPECT_EQ(VkDevice->_pipelineLayoutCache->GetEntryCount(), count);
}

TEST_F(VulkanDeviceFixture, NativeLayoutCacheIncludesImmutableSamplerState) {
    if (!Available) GTEST_SKIP() << "no vulkan device";
    auto desc = ResolveFixture("shadow_static_sampler");
    ASSERT_TRUE(desc);
    ASSERT_FALSE(desc->ImmutableSamplers.empty());
    auto first = VkDevice->CreatePipelineLayout(*desc);
    auto second = VkDevice->CreatePipelineLayout(*desc);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(CastVkObject(first.Get())->GetNative(), CastVkObject(second.Get())->GetNative());
    desc->ImmutableSamplers[0].MipLodBias += 1.0f;
    auto changed = VkDevice->CreatePipelineLayout(*desc);
    ASSERT_TRUE(changed);
    EXPECT_NE(CastVkObject(first.Get())->GetNative(), CastVkObject(changed.Get())->GetNative());
}

TEST_F(VulkanDeviceFixture, NativeLayoutCacheRollsBackNativeFailureAndCanRetry) {
    if (!Available) GTEST_SKIP() << "no vulkan device";
    auto create = VkDevice->_ftb.vkCreatePipelineLayout;
    VkDevice->_ftb.vkCreatePipelineLayout = [](::VkDevice, const VkPipelineLayoutCreateInfo*, const VkAllocationCallbacks*, VkPipelineLayout*) -> VkResult {
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    };
    auto failed = VkDevice->CreatePipelineLayout(MakeMixedLayout());
    VkDevice->_ftb.vkCreatePipelineLayout = create;
    EXPECT_FALSE(failed);
    EXPECT_EQ(VkDevice->_pipelineLayoutCache->GetEntryCount(), 0u);
    EXPECT_EQ(VkDevice->_descriptorSetLayoutCache.GetLayoutCount(), 0u);
    auto retry = VkDevice->CreatePipelineLayout(MakeMixedLayout());
    ASSERT_TRUE(retry);
    EXPECT_EQ(VkDevice->_pipelineLayoutCache->GetEntryCount(), 1u);
}

TEST_F(VulkanDeviceFixture, NativeLayoutCacheDoesNotShareAcrossDevices) {
    if (!Available) GTEST_SKIP() << "no vulkan device";
    const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, 1}};
    auto otherResult = Device::Create(DeviceDescriptor{VulkanDeviceDescriptor{.Queues = queues}});
    ASSERT_TRUE(otherResult);
    auto other = otherResult.Release();
    auto* otherVk = static_cast<DeviceVulkan*>(other.get());
    auto first = VkDevice->CreatePipelineLayout(MakeMixedLayout());
    auto second = otherVk->CreatePipelineLayout(MakeMixedLayout());
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(CastVkObject(first.Get())->_nativeLayout, CastVkObject(second.Get())->_nativeLayout);
    EXPECT_EQ(VkDevice->_pipelineLayoutCache->GetEntryCount(), 1u);
    EXPECT_EQ(otherVk->_pipelineLayoutCache->GetEntryCount(), 1u);
    first = nullptr;
    EXPECT_EQ(VkDevice->_pipelineLayoutCache->GetEntryCount(), 0u);
    EXPECT_EQ(otherVk->_pipelineLayoutCache->GetEntryCount(), 1u);
    EXPECT_TRUE(second.Get()->IsValid());
}

TEST(PipelineLayoutKeyVulkanTest, FullEqualitySeparatesFlagsAndPushRangesDespiteHashCollisions) {
    struct CollidingHash {
        size_t operator()(const vulkan::PipelineLayoutKeyVulkan&) const noexcept { return 0; }
    };
    vulkan::PipelineLayoutKeyVulkan base;
    base.PushRanges.push_back({VK_SHADER_STAGE_VERTEX_BIT, 0, 16});
    auto offset = base;
    offset.PushRanges[0].Offset = 4;
    auto flags = base;
    flags.Flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
    unordered_map<vulkan::PipelineLayoutKeyVulkan, uint32_t, CollidingHash> map;
    map.emplace(base, 1);
    map.emplace(offset, 2);
    map.emplace(flags, 3);
    EXPECT_EQ(map.size(), 3u);
    EXPECT_EQ(map.at(base), 1u);
    EXPECT_EQ(map.at(offset), 2u);
    EXPECT_EQ(map.at(flags), 3u);
}

}  // namespace radray::render

#endif
