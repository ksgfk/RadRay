#pragma once

#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>

#include <radray/shader/shader_compiler_contract.h>

namespace radray::render::test {

enum class FixtureResourceKind : uint8_t {
    Texture,
    Sampler,
    CBuffer,
    RootConstant,
    StorageBuffer,
};

struct FixtureEntryFact {
    std::string_view Name;
    shader::ShaderStage Stage{shader::ShaderStage::Vertex};
};

struct FixtureBindingFact {
    std::string_view Name;
    FixtureResourceKind Kind{FixtureResourceKind::Texture};
    uint32_t D3D12Group{0};
    uint32_t D3D12Binding{0};
    uint32_t VulkanSet{0};
    uint32_t VulkanBinding{0};
    uint32_t StageMask{0};
    // A policy static sampler lands differently on each target by design: on DXIL it owns no
    // descriptor table slot and lives only in the serialized carrier, while on SPIR-V the same
    // policy slot arrives as a table entry plus a published immutable sampler state.
    bool PolicyStaticSampler{false};
    std::string_view PayloadType{};
};

// Logical resource kinds are persisted wire values, so the fixture table names them once instead of
// letting every test spell the numbering out again.
inline constexpr shader::ShaderBindingKind ExpectedWireKind(FixtureResourceKind kind) noexcept {
    switch (kind) {
        case FixtureResourceKind::Texture: return shader::ShaderBindingKind::Texture;
        case FixtureResourceKind::Sampler: return shader::ShaderBindingKind::Sampler;
        case FixtureResourceKind::StorageBuffer: return shader::ShaderBindingKind::RWStructuredBuffer;
        case FixtureResourceKind::CBuffer:
        case FixtureResourceKind::RootConstant: break;
    }
    return shader::ShaderBindingKind::CBuffer;
}

struct ShaderContractFixture {
    std::string_view Name;
    std::string_view SourcePath;
    shader::ShaderKind Kind{shader::ShaderKind::Graphics};
    std::span<const FixtureEntryFact> Entries;
    std::span<const FixtureBindingFact> Bindings;
    uint32_t TypeRecordCount{0};
    bool HasMultipleRootConstants{false};
    bool HasSingleSpirvPushBlock{false};
    uint32_t SpirvTypeRecordCount{0};
};

inline constexpr FixtureEntryFact kNoResourceEntries[] = {
    {"VSMain", shader::ShaderStage::Vertex},
    {"PSMain", shader::ShaderStage::Pixel},
};
inline constexpr FixtureEntryFact kVertexOnlyEntries[] = {
    {"VSMain", shader::ShaderStage::Vertex},
};
inline constexpr FixtureEntryFact kDepthOnlyEntries[] = {
    {"VSMain", shader::ShaderStage::Vertex},
};
inline constexpr FixtureEntryFact kComputeEntries[] = {
    {"CSMain", shader::ShaderStage::Compute},
};

inline constexpr FixtureBindingFact kTextureSamplerBindings[] = {
    {"AlbedoTexture", FixtureResourceKind::Texture, 0, 0, 1, 7, 0x2},
    {"LinearSampler", FixtureResourceKind::Sampler, 0, 0, 1, 8, 0x2},
};
inline constexpr FixtureBindingFact kShadowStaticSamplerBindings[] = {
    {"ShadowTexture", FixtureResourceKind::Texture, 0, 0, 4, 1, 0x2},
    {"ShadowSampler", FixtureResourceKind::Sampler, 0, 0, 4, 2, 0x2, true},
};
inline constexpr FixtureBindingFact kRootConstantBindings[] = {
    {"ObjectConstants", FixtureResourceKind::RootConstant, 0, 0, 0, 0, 0x3},
    {"MaterialConstants", FixtureResourceKind::RootConstant, 0, 1, 0, 0, 0x3},
};
inline constexpr FixtureBindingFact kPushConstantBindings[] = {
    {"PushConstants", FixtureResourceKind::RootConstant, 0, 0, 0, 0, 0x3, false, "PushData"},
};
inline constexpr FixtureBindingFact kTargetSpecificBindings[] = {
    {"TargetTexture", FixtureResourceKind::Texture, 0, 0, 5, 2, 0x2},
    {"TargetSampler", FixtureResourceKind::Sampler, 0, 0, 5, 3, 0x2},
};
inline constexpr FixtureBindingFact kNestedTypeBindings[] = {
    {"Constants", FixtureResourceKind::CBuffer, 0, 0, 0, 0, 0x3, false, "NestedRoot"},
};
inline constexpr FixtureBindingFact kMultipleCBufferBindings[] = {
    {"First", FixtureResourceKind::CBuffer, 0, 0, 0, 0, 0x3, false, "FirstRoot"},
    {"Second", FixtureResourceKind::CBuffer, 1, 0, 1, 0, 0x2, false, "SecondRoot"},
};
inline constexpr FixtureBindingFact kSharedCBufferTypeBindings[] = {
    {"First", FixtureResourceKind::CBuffer, 0, 0, 0, 0, 0x3, false, "SharedRoot"},
    {"Second", FixtureResourceKind::CBuffer, 1, 0, 1, 0, 0x2, false, "SharedRoot"},
};
inline constexpr FixtureBindingFact kNestedCBufferRootBindings[] = {
    {"Inner", FixtureResourceKind::CBuffer, 0, 0, 0, 0, 0x1, false, "InnerRoot"},
    {"Outer", FixtureResourceKind::CBuffer, 1, 0, 1, 0, 0x2, false, "OuterRoot"},
};
inline constexpr FixtureBindingFact kComputeBindings[] = {
    {"Output", FixtureResourceKind::StorageBuffer, 0, 2, 0, 6, 0x4},
};

inline constexpr ShaderContractFixture kShaderContractFixtures[] = {
    {"no_resource_graphics", "fixtures/no_resource_graphics.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, {}, 0, false, false},
    {"vertex_only", "fixtures/vertex_only.hlsl", shader::ShaderKind::Graphics, kVertexOnlyEntries, {}, 0, false, false},
    {"depth_only", "fixtures/depth_only.hlsl", shader::ShaderKind::Graphics, kDepthOnlyEntries, {}, 0, false, false},
    {"texture_sampler", "fixtures/texture_sampler.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kTextureSamplerBindings, 0, false, false},
    {"shadow_static_sampler", "fixtures/shadow_static_sampler.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kShadowStaticSamplerBindings, 0, false, false},
    {"multiple_root_constants", "fixtures/multiple_root_constants.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kRootConstantBindings, 0, true, false},
    {"spirv_push_constant", "fixtures/spirv_push_constant.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kPushConstantBindings, 0, false, true, 2},
    {"target_specific_bindings", "fixtures/target_specific_bindings.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kTargetSpecificBindings, 0, false, false},
    {"nested_types", "fixtures/nested_types.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kNestedTypeBindings, 8, false, false},
    {"multiple_cbuffers", "fixtures/multiple_cbuffers.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kMultipleCBufferBindings, 6, false, false, 6},
    {"compute", "fixtures/compute.hlsl", shader::ShaderKind::Compute, kComputeEntries, kComputeBindings, 0, false, false},
    {"unused_resource", "fixtures/unused_resource.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, {}, 0, false, false},
    {"shared_cbuffer_type", "fixtures/shared_cbuffer_type.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kSharedCBufferTypeBindings, 2, false, false},
    {"nested_cbuffer_roots", "fixtures/nested_cbuffer_roots.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kNestedCBufferRootBindings, 4, false, false},
};

inline constexpr shader::Hash128 kExpectedGpuArtifacts[14][2] = {
    {{{0x66, 0x02, 0x18, 0x73, 0x2d, 0xba, 0x6c, 0xb1, 0xb7, 0x5a, 0x93, 0xab, 0xf2, 0x59, 0x8a, 0xc7}},
     {{0x0c, 0x12, 0xe3, 0x59, 0x49, 0xf6, 0x78, 0x86, 0x51, 0xec, 0x78, 0x69, 0x12, 0x4b, 0x4f, 0xde}}},  // no_resource_graphics
    {{{0xf2, 0x10, 0x83, 0x7e, 0x6a, 0x22, 0xfc, 0xaa, 0x6d, 0xe3, 0x47, 0xe4, 0xd5, 0x2d, 0xa2, 0x80}},
     {{0x1a, 0x0a, 0x3c, 0xa3, 0x4e, 0x5c, 0xf9, 0xe5, 0xd9, 0x3b, 0x8e, 0x10, 0x25, 0x71, 0x51, 0x0a}}},  // vertex_only
    {{{0xf2, 0x10, 0x83, 0x7e, 0x6a, 0x22, 0xfc, 0xaa, 0x6d, 0xe3, 0x47, 0xe4, 0xd5, 0x2d, 0xa2, 0x80}},
     {{0x1a, 0x0a, 0x3c, 0xa3, 0x4e, 0x5c, 0xf9, 0xe5, 0xd9, 0x3b, 0x8e, 0x10, 0x25, 0x71, 0x51, 0x0a}}},  // depth_only
    {{{0xee, 0x25, 0x40, 0x4b, 0xae, 0x76, 0xa7, 0xdf, 0xb5, 0x23, 0xfc, 0xed, 0xaa, 0x74, 0x20, 0x61}},
     {{0xfe, 0xf3, 0x41, 0x1c, 0x46, 0x53, 0xe0, 0xae, 0x15, 0x25, 0x61, 0xbb, 0xc7, 0xc4, 0x97, 0xa9}}},  // texture_sampler
    {{{0x08, 0x5a, 0x33, 0x91, 0x0d, 0xc8, 0x5d, 0x51, 0xaf, 0x13, 0xf6, 0x91, 0xde, 0x84, 0x02, 0x6a}},
     {{0x1c, 0xb8, 0x6f, 0x7d, 0xc1, 0x90, 0xcd, 0xbd, 0xdb, 0xc9, 0x9e, 0xfb, 0x75, 0x6b, 0x76, 0x96}}},  // shadow_static_sampler
    {{{0xc1, 0x55, 0xf8, 0x42, 0x4e, 0xed, 0x76, 0x9c, 0x22, 0xa9, 0xf6, 0xab, 0x86, 0x5b, 0x1c, 0xd6}},
     {{0x84, 0x48, 0xbe, 0x4b, 0x9e, 0xfe, 0xb9, 0xc2, 0x89, 0xee, 0x68, 0xc5, 0xfb, 0x5f, 0xe5, 0xd0}}},  // multiple_root_constants
    {{{0xdc, 0x67, 0xb8, 0xfd, 0x73, 0xe3, 0xa6, 0x78, 0xb1, 0x81, 0x0a, 0x32, 0xb1, 0xe8, 0xb6, 0x78}},
     {{0x41, 0xea, 0xa6, 0x19, 0xe6, 0x19, 0x56, 0x2c, 0x83, 0x1a, 0xd9, 0xae, 0xbf, 0x8f, 0x51, 0x68}}},  // spirv_push_constant
    {{{0xaa, 0xf3, 0x63, 0x5d, 0xec, 0x62, 0x0e, 0x3a, 0xb5, 0x1b, 0x85, 0xc5, 0xed, 0x5d, 0xea, 0x0e}},
     {{0x79, 0x57, 0xd9, 0xaf, 0x07, 0xd8, 0xe7, 0xd3, 0xe4, 0xb9, 0x34, 0x6a, 0xa6, 0x93, 0xe3, 0xc9}}},  // target_specific_bindings
    {{{0xa2, 0x2b, 0xa2, 0x0f, 0x7d, 0x0c, 0xd8, 0x06, 0x05, 0xfb, 0x27, 0x22, 0x09, 0x5f, 0x60, 0xb1}},
     {{0x30, 0x86, 0x51, 0x09, 0x24, 0x97, 0x1a, 0x41, 0xbf, 0x83, 0x28, 0x99, 0x42, 0x38, 0xab, 0x41}}},  // nested_types
    {{{0xd5, 0x98, 0x18, 0xc5, 0x1b, 0x8a, 0xbd, 0x26, 0x08, 0xa6, 0x4b, 0x72, 0xc5, 0xcf, 0x82, 0x4c}},
     {{0xa9, 0x49, 0x5c, 0x98, 0xaa, 0x6f, 0x07, 0x02, 0x28, 0xb6, 0x69, 0xd1, 0x16, 0x91, 0xf7, 0xd7}}},  // multiple_cbuffers
    {{{0xf7, 0xae, 0x9c, 0xbe, 0x1a, 0xc3, 0x30, 0xe0, 0x60, 0x27, 0x73, 0x88, 0xbf, 0x7c, 0x11, 0x14}},
     {{0xeb, 0x79, 0xca, 0x0b, 0x02, 0x13, 0xf5, 0x72, 0xfc, 0x73, 0x8b, 0xc0, 0xef, 0x86, 0x2c, 0x50}}},  // compute
    {{{0xe7, 0x5b, 0x54, 0xa9, 0x34, 0x1f, 0x49, 0xb9, 0x50, 0xe9, 0xc0, 0x21, 0xdc, 0x18, 0x7d, 0xd7}},
     {{0x9c, 0x57, 0x26, 0xe2, 0x2f, 0xca, 0xe1, 0xa3, 0x59, 0xcd, 0x7b, 0xc3, 0x34, 0x1c, 0xcf, 0x06}}},  // unused_resource
    {{{0x82, 0x96, 0xfa, 0x48, 0x77, 0x4a, 0xba, 0x54, 0xcd, 0xf1, 0x47, 0x18, 0x6f, 0xc7, 0xa0, 0x6e}},
     {{0x77, 0x4f, 0xbb, 0x69, 0x76, 0x37, 0x31, 0x4e, 0x76, 0x5d, 0x73, 0xe6, 0xd0, 0xe3, 0xcc, 0x43}}},  // shared_cbuffer_type
    {{{0x79, 0xb0, 0xbd, 0x5c, 0x4f, 0xd6, 0x4d, 0xe6, 0x46, 0x25, 0x28, 0xac, 0x4b, 0xd9, 0x7f, 0x93}},
     {{0x16, 0xb6, 0xb0, 0xbc, 0x21, 0x9d, 0xf7, 0x7d, 0x3b, 0xc1, 0xbe, 0xf9, 0x89, 0xc0, 0x01, 0x97}}},  // nested_cbuffer_roots
};

// Golden hashes are indexed by fixture position, so the two tables must stay the same
// length. Adding a fixture without its hashes is a compile error, not a decode failure.
static_assert(
    std::size(kExpectedGpuArtifacts) == std::size(kShaderContractFixtures),
    "every shader contract fixture needs one golden GPU artifact hash pair");

inline constexpr shader::GpuArtifactHash ExpectedGpuArtifact(
    size_t fixtureIndex,
    shader::ShaderTarget target) noexcept {
    return kExpectedGpuArtifacts[fixtureIndex][static_cast<uint8_t>(target)];
}

inline constexpr std::span<const ShaderContractFixture> GetShaderContractFixtures() noexcept {
    return kShaderContractFixtures;
}

}  // namespace radray::render::test
