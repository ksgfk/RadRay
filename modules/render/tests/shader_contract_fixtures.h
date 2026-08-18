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
};

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
    {"ShadowSampler", FixtureResourceKind::Sampler, 0, 0, 4, 2, 0x2},
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
    {{{0x9e, 0x3d, 0x5f, 0xa7, 0x31, 0x08, 0xd4, 0x76, 0x4f, 0x32, 0xdd, 0x49, 0x78, 0x88, 0xe7, 0xc6}},
     {{0xd4, 0xbe, 0xf1, 0xdd, 0xf9, 0xe6, 0xbd, 0x14, 0x79, 0xa9, 0x8d, 0xe0, 0x05, 0x69, 0xb3, 0xa1}}},
    {{{0x1a, 0x64, 0x64, 0x3b, 0x96, 0xaa, 0x73, 0xe6, 0xd5, 0x31, 0x40, 0xfd, 0x5c, 0x0a, 0x24, 0x6a}},
     {{0x22, 0x21, 0x3e, 0x06, 0x2e, 0x4f, 0xb0, 0xd3, 0x51, 0x05, 0xb7, 0x0f, 0xb3, 0xd2, 0x2b, 0x92}}},
    {{{0x1a, 0x64, 0x64, 0x3b, 0x96, 0xaa, 0x73, 0xe6, 0xd5, 0x31, 0x40, 0xfd, 0x5c, 0x0a, 0x24, 0x6a}},
     {{0x22, 0x21, 0x3e, 0x06, 0x2e, 0x4f, 0xb0, 0xd3, 0x51, 0x05, 0xb7, 0x0f, 0xb3, 0xd2, 0x2b, 0x92}}},
    {{{0xb6, 0x78, 0x2d, 0xd7, 0x07, 0x1c, 0xb9, 0xcf, 0x45, 0x93, 0x2a, 0x6d, 0x1d, 0x0d, 0x3c, 0xf0}},
     {{0x6e, 0xa2, 0x99, 0xcd, 0x2f, 0x65, 0x9b, 0xfd, 0x6d, 0xdb, 0xd4, 0xb5, 0xfd, 0xb8, 0x7a, 0xd7}}},
    {{{0x5f, 0x45, 0x10, 0x1b, 0xf7, 0xe6, 0x29, 0xbb, 0xde, 0x61, 0xba, 0xf7, 0xd0, 0xdc, 0xa8, 0x95}},
     {{0x21, 0x1c, 0xe0, 0x2a, 0x22, 0x5b, 0xef, 0xa5, 0xe0, 0x83, 0x39, 0x38, 0x53, 0x98, 0x6d, 0xca}}},
    {{{0x56, 0xab, 0xc3, 0x56, 0xa3, 0xb3, 0xc2, 0x49, 0x1f, 0x27, 0xd2, 0x57, 0xcc, 0x93, 0x20, 0x3b}},
     {{0x70, 0x0f, 0xc6, 0x2a, 0x7d, 0x60, 0xfc, 0x52, 0x45, 0xe0, 0x9e, 0x83, 0xf6, 0x78, 0xd9, 0x2d}}},
    {{{0xb4, 0xd8, 0xa0, 0xfd, 0x40, 0x0b, 0xaf, 0x99, 0x19, 0xa0, 0x60, 0x75, 0x27, 0xc3, 0x2a, 0x2c}},
     {{0x9f, 0x72, 0x4b, 0x57, 0x01, 0x62, 0xaf, 0x5f, 0x54, 0xa4, 0xb9, 0x74, 0x1a, 0xbd, 0xbe, 0xb9}}},
    {{{0x02, 0x12, 0x3a, 0xe6, 0x1a, 0x45, 0x7f, 0x09, 0x05, 0xdf, 0x64, 0x80, 0x77, 0x37, 0xcf, 0xfd}},
     {{0x09, 0x96, 0xca, 0x47, 0x18, 0xef, 0x1d, 0xf9, 0x64, 0xb7, 0xb5, 0xe9, 0x12, 0x79, 0xbc, 0xc9}}},
    {{{0xa5, 0xbd, 0x15, 0x8c, 0x47, 0x80, 0xb7, 0x0c, 0xc4, 0x8e, 0x74, 0xb3, 0x92, 0xbb, 0x19, 0x81}},
     {{0x83, 0x2c, 0x9d, 0x8a, 0xc8, 0x9f, 0x99, 0x14, 0xca, 0xa9, 0xf7, 0x16, 0x5b, 0x24, 0xc4, 0x55}}},
    {{{0x59, 0x9d, 0x02, 0xdc, 0xee, 0xc8, 0xe0, 0x8b, 0xc0, 0x61, 0x5e, 0x06, 0x0f, 0xea, 0x24, 0x88}},
     {{0xb9, 0xea, 0x50, 0x6d, 0xdc, 0xc7, 0x62, 0xca, 0x98, 0xe0, 0x08, 0x97, 0x80, 0x88, 0x2d, 0xbb}}},
    {{{0x39, 0x76, 0x18, 0x2d, 0x73, 0x76, 0x46, 0xbf, 0x6a, 0x84, 0x84, 0x17, 0x6c, 0x04, 0x44, 0x94}},
     {{0x95, 0x7a, 0xdf, 0xff, 0x2e, 0xb0, 0xc2, 0x0c, 0x96, 0x15, 0xcd, 0x3d, 0xa5, 0x67, 0xe7, 0x1b}}},
    {{{0x8f, 0xf9, 0xfd, 0x8a, 0x1d, 0x75, 0xc7, 0xca, 0xd8, 0xed, 0xf0, 0x07, 0x9c, 0x5d, 0x17, 0x6c}},
     {{0xc4, 0x15, 0xd1, 0x69, 0x0e, 0x75, 0x05, 0x39, 0xc1, 0x66, 0x44, 0x51, 0x8a, 0x45, 0x24, 0x3c}}},
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
