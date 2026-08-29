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
};

// Logical resource kinds are persisted wire values, so the fixture table names them once instead of
// letting every test spell the numbering out again.
inline constexpr shader::ShaderBindingKind ExpectedWireKind(FixtureResourceKind kind) noexcept {
    switch (kind) {
        case FixtureResourceKind::Texture: return shader::ShaderBindingKind::Texture;
        case FixtureResourceKind::Sampler: return shader::ShaderBindingKind::Sampler;
        case FixtureResourceKind::StorageBuffer: return shader::ShaderBindingKind::RWStructuredBuffer;
        case FixtureResourceKind::CBuffer: case FixtureResourceKind::RootConstant: break;
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
    {"PushData", FixtureResourceKind::RootConstant, 0, 0, 0, 0, 0x3},
};
inline constexpr FixtureBindingFact kTargetSpecificBindings[] = {
    {"TargetTexture", FixtureResourceKind::Texture, 0, 0, 5, 2, 0x2},
    {"TargetSampler", FixtureResourceKind::Sampler, 0, 0, 5, 3, 0x2},
};
inline constexpr FixtureBindingFact kNestedTypeBindings[] = {
    {"Constants", FixtureResourceKind::CBuffer, 0, 0, 0, 0, 0x3},
};
inline constexpr FixtureBindingFact kMultipleCBufferBindings[] = {
    {"First", FixtureResourceKind::CBuffer, 0, 0, 0, 0, 0x3},
    {"Second", FixtureResourceKind::CBuffer, 1, 0, 1, 0, 0x2},
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
    {"multiple_cbuffers", "fixtures/multiple_cbuffers.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, kMultipleCBufferBindings, 8, false, false, 6},
    {"compute", "fixtures/compute.hlsl", shader::ShaderKind::Compute, kComputeEntries, kComputeBindings, 0, false, false},
    {"unused_resource", "fixtures/unused_resource.hlsl", shader::ShaderKind::Graphics, kNoResourceEntries, {}, 0, false, false},
};

inline constexpr shader::Hash128 kExpectedGpuArtifacts[12][2] = {
    {{{0x63, 0x45, 0x9e, 0xad, 0xe1, 0x83, 0x46, 0x2f, 0x58, 0x2a, 0x06, 0x5f, 0x01, 0xb1, 0xe1, 0x76}},
     {{0x0c, 0x12, 0xe3, 0x59, 0x49, 0xf6, 0x78, 0x86, 0x51, 0xec, 0x78, 0x69, 0x12, 0x4b, 0x4f, 0xde}}},
    {{{0x5f, 0x7b, 0xa0, 0x10, 0x0c, 0xa2, 0x77, 0xd8, 0x82, 0x8c, 0x49, 0xdc, 0xcf, 0x3e, 0xe6, 0x6a}},
     {{0x1a, 0x0a, 0x3c, 0xa3, 0x4e, 0x5c, 0xf9, 0xe5, 0xd9, 0x3b, 0x8e, 0x10, 0x25, 0x71, 0x51, 0x0a}}},
    {{{0x5f, 0x7b, 0xa0, 0x10, 0x0c, 0xa2, 0x77, 0xd8, 0x82, 0x8c, 0x49, 0xdc, 0xcf, 0x3e, 0xe6, 0x6a}},
     {{0x1a, 0x0a, 0x3c, 0xa3, 0x4e, 0x5c, 0xf9, 0xe5, 0xd9, 0x3b, 0x8e, 0x10, 0x25, 0x71, 0x51, 0x0a}}},
    {{{0xdb, 0xc5, 0x89, 0x4b, 0xab, 0x4c, 0x23, 0xdb, 0x02, 0xc1, 0x56, 0xa6, 0x23, 0x80, 0xae, 0xd6}},
     {{0x56, 0x72, 0x27, 0xd5, 0x57, 0x51, 0xed, 0x80, 0xad, 0xb2, 0xe2, 0x3b, 0xa4, 0x23, 0xd0, 0x81}}},
    {{{0x1e, 0xcd, 0xa3, 0x51, 0x57, 0x0e, 0xd4, 0x3f, 0xf5, 0xbe, 0xcb, 0xbc, 0x2b, 0xc2, 0x22, 0x19}},
     {{0xc4, 0xe4, 0x73, 0xed, 0x58, 0x2c, 0x13, 0x75, 0x33, 0x87, 0x15, 0x26, 0x1f, 0x02, 0x90, 0x51}}},
    {{{0x6e, 0x0a, 0x40, 0xa5, 0xe1, 0x33, 0x13, 0x00, 0x67, 0xd9, 0xb2, 0x7f, 0xca, 0x1f, 0xfa, 0x6b}},
     {{0x84, 0x48, 0xbe, 0x4b, 0x9e, 0xfe, 0xb9, 0xc2, 0x89, 0xee, 0x68, 0xc5, 0xfb, 0x5f, 0xe5, 0xd0}}},
    {{{0x0b, 0x04, 0x14, 0x58, 0x17, 0x91, 0x8c, 0x57, 0x04, 0xde, 0xf5, 0x6d, 0xe2, 0x33, 0xed, 0x6e}},
     {{0x29, 0x6c, 0xed, 0x7a, 0x73, 0xaa, 0x36, 0x48, 0x03, 0xe8, 0x08, 0x74, 0xe4, 0x19, 0xba, 0x96}}},
    {{{0x5b, 0xf4, 0x5f, 0xf7, 0x84, 0x7c, 0x11, 0x79, 0x0a, 0x03, 0x9f, 0x86, 0x06, 0xf1, 0xf0, 0xdd}},
     {{0x61, 0x70, 0x1f, 0x3d, 0x31, 0x2b, 0xf7, 0x33, 0x9c, 0x4d, 0xa3, 0xd0, 0x86, 0xe4, 0xba, 0xd6}}},
    {{{0x75, 0x45, 0x88, 0x3d, 0xce, 0xbd, 0x92, 0xea, 0x8c, 0x6e, 0xd1, 0xe2, 0x25, 0xc1, 0x9b, 0x39}},
     {{0xf7, 0xc4, 0x5a, 0x0c, 0xe6, 0x8d, 0x71, 0x13, 0xa6, 0x5b, 0x2d, 0x43, 0x51, 0xdb, 0xd4, 0x89}}},
    {{{0x04, 0xdc, 0xb4, 0x35, 0x85, 0x02, 0x57, 0x5d, 0xff, 0xe4, 0x18, 0x04, 0xbd, 0xb6, 0xdf, 0xe0}},
     {{0xf9, 0x27, 0x32, 0xeb, 0xf0, 0x58, 0xc0, 0x56, 0xb8, 0x7e, 0x3b, 0x46, 0xe1, 0x08, 0x27, 0xa2}}},
    {{{0x24, 0x8a, 0x06, 0x4b, 0x7f, 0x1d, 0xa7, 0x24, 0x65, 0x3e, 0x5c, 0xc6, 0x32, 0x96, 0xf7, 0xd6}},
     {{0xcf, 0xa9, 0xce, 0xbc, 0x08, 0xf2, 0xa0, 0xf0, 0xa0, 0x7a, 0x7a, 0x1b, 0xa6, 0x9c, 0x02, 0xc6}}},
    {{{0x88, 0xc4, 0x7e, 0x42, 0x0c, 0x42, 0xaf, 0x3b, 0x95, 0xb9, 0x0c, 0xde, 0xb3, 0xe2, 0xb7, 0x9f}},
     {{0x9c, 0x57, 0x26, 0xe2, 0x2f, 0xca, 0xe1, 0xa3, 0x59, 0xcd, 0x7b, 0xc3, 0x34, 0x1c, 0xcf, 0x06}}},
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
