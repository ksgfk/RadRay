#include "known_gpu_issues.h"
#include "external_texture_test_utils.h"

#include <fstream>
#include <filesystem>
#include <tuple>

namespace radray::render::test {
namespace {
TEST(KnownGpuIssueContract, MatchesRecordedCasesAndRejectsNeighboringDescriptions) {
    std::ifstream fixture(std::filesystem::path{RADRAY_PROJECT_DIR} / "modules/render/tests/data/known_gpu_issue_cases.tsv");
    ASSERT_TRUE(fixture);
    vector<std::tuple<string, int, KnownGpuIssue>> recorded;
    for (string line; std::getline(fixture, line);) {
        if (line.empty() || line[0] == '#') continue;
        const auto first = line.find('\t'), last = line.rfind('\t');
        ASSERT_NE(first, string::npos);
        ASSERT_EQ(last, first + 2);
        recorded.emplace_back(line.substr(0, first), line[first + 1] - '0', KnownGpuIssue(line[last + 1] - '0'));
    }
    ASSERT_EQ(recorded.size(), 209u);
    const GpuTestEnvironment dx{RenderBackend::D3D12, 0x10de, 0x2704, 0x0020000000100835ull};
    const GpuTestEnvironment vk{RenderBackend::Vulkan, 0x10de, 0x2704, 0x9b404000};
    const ExternalSupport supported{true, true, {}};
    uint32_t matched = 0;
    for (const auto& item : ExternalTextureCases()) {
        for (int route = 0; route < 5; ++route) {
            const auto& source = route < 2 ? dx : vk;
            const auto& target = route < 4 && route % 2 == 0 ? dx : vk;
            const ExternalTextureDescriptor options{route == 4 ? ExternalMemoryHandleType::OpaqueWin32 : ExternalMemoryHandleType::D3D12Resource, item.Reinterpret};
            const auto known = std::ranges::find_if(recorded, [&](const auto& row) { return std::get<0>(row) == item.Name && std::get<1>(row) == route; });
            const auto expected = known == recorded.end() ? KnownGpuIssue::None : std::get<2>(*known);
            SCOPED_TRACE(fmt::format("{} route={}", item.Name, route));
            const auto match = [&](const TextureDescriptor& desc, const ExternalTextureDescriptor& opts) {
                return FindExternalTextureKnownIssue(source, target, desc, opts, item.Use, supported, supported);
            };
            ASSERT_EQ(match(item.Descriptor, options), expected);
            if (expected == KnownGpuIssue::None) continue;
            ++matched;
            for (uint32_t mutation = 0; mutation < 7; ++mutation) {
                auto d = item.Descriptor;
                switch (mutation) {
                    case 0: ++d.Width; break;
                    case 1: ++d.Height; break;
                    case 2: ++d.DepthOrArraySize; break;
                    case 3: ++d.MipLevels; break;
                    case 4: d.SampleCount = 4; break;
                    case 5: d.Memory = MemoryType::Upload; break;
                    case 6: d.Usage = {}; break;
                }
                EXPECT_EQ(match(d, options), KnownGpuIssue::None) << mutation;
            }
            EXPECT_EQ(match(item.Descriptor, {ExternalMemoryHandleType::OpaqueWin32, item.Reinterpret}), KnownGpuIssue::None);
            EXPECT_EQ(match(item.Descriptor, {options.HandleType, !item.Reinterpret}), KnownGpuIssue::None);
            EXPECT_EQ(FindExternalTextureKnownIssue(source, target, item.Descriptor, options, item.Use, supported, supported, true), KnownGpuIssue::None);
        }
    }
    EXPECT_EQ(matched, recorded.size());
}

TEST(KnownGpuIssueContract, UnknownEnvironmentAndQueryErrorsNeverMatch) {
    const GpuTestEnvironment dx{RenderBackend::D3D12, 0x10de, 0x2704, 0x0020000000100835ull};
    const GpuTestEnvironment vk{RenderBackend::Vulkan, 0x10de, 0x2704, 0x9b404000};
    const TextureDescriptor d{TextureDimension::Dim2DArray, 9, 5, 2, 2, 1, TextureFormat::RGBA8_UNORM, MemoryType::Device, TextureUse::CopySource | TextureUse::CopyDestination};
    const ExternalSupport supported{true, true, {}};
    const ExternalSupport importOnly{false, true, {"export unsupported"}};
    const auto match = [&](const GpuTestEnvironment& a, const GpuTestEnvironment& b, const ExternalSupport& sa, const ExternalSupport& sb) {
        return FindExternalTextureKnownIssue(a, b, d, {}, ExternalTextureTestUse::Copy, sa, sb);
    };
    EXPECT_EQ(match(dx, vk, supported, importOnly), KnownGpuIssue::PartialMipArray);
    for (uint32_t mutation = 0; mutation < 4; ++mutation) {
        auto a = dx, b = vk;
        for (auto* environment : {&a, &b}) {
            switch (mutation) {
                case 0: environment->VendorId = 0x8086; break;
                case 1: ++environment->DeviceId; break;
                case 2: ++environment->DriverVersion; break;
                case 3: environment->DriverVersion = 0; break;
            }
        }
        EXPECT_EQ(match(a, vk, supported, supported), KnownGpuIssue::None);
        EXPECT_EQ(match(dx, b, supported, supported), KnownGpuIssue::None);
    }
    EXPECT_EQ(match(vk, dx, supported, supported), KnownGpuIssue::None);
    for (auto failure : {ExternalSupport{false, false, {}}, ExternalSupport{true, true, {"query failed", ExternalNativeApi::Vulkan, -2}},
                         ExternalSupport{false, false, {"format unsupported", ExternalNativeApi::Vulkan, -11}},
                         ExternalSupport{true, true, {"query failed", ExternalNativeApi::D3D12, -2147024809LL}}}) {
        EXPECT_EQ(match(dx, vk, failure, supported), KnownGpuIssue::None);
        EXPECT_EQ(match(dx, vk, supported, failure), KnownGpuIssue::None);
    }
    EXPECT_TRUE(ShouldSkipKnownGpuIssue(KnownGpuIssue::PartialMipArray, false));
    EXPECT_FALSE(ShouldSkipKnownGpuIssue(KnownGpuIssue::PartialMipArray, true));
    EXPECT_FALSE(ShouldSkipKnownGpuIssue(KnownGpuIssue::None, false));
}

TEST(KnownGpuIssueContract, NativeAndIpcDescriptionsAreBounded) {
    const GpuTestEnvironment dx{RenderBackend::D3D12, 0x10de, 0x2704, 0x0020000000100835ull};
    const GpuTestEnvironment vk{RenderBackend::Vulkan, 0x10de, 0x2704, 0x9b404000};
    const ExternalSupport supported{true, true, {}};
    const auto copy = TextureUse::CopySource | TextureUse::CopyDestination;
    TextureDescriptor ipc{TextureDimension::Dim2DArray, 35, 19, 3, 3, 1, TextureFormat::RGBA16_FLOAT, MemoryType::Device, copy, ResourceHint::Dedicated};
    EXPECT_EQ(FindExternalTextureKnownIssue(dx, vk, ipc, {}, ExternalTextureTestUse::Copy, supported, supported), KnownGpuIssue::PartialMipArray);
    ++ipc.Width;
    EXPECT_EQ(FindExternalTextureKnownIssue(dx, vk, ipc, {}, ExternalTextureTestUse::Copy, supported, supported), KnownGpuIssue::None);
    for (const uint32_t samples : {2u, 4u, 8u, 16u}) {
        TextureDescriptor native{TextureDimension::Dim2D, 8, 8, 1, 1, samples, TextureFormat::RGBA8_UNORM, MemoryType::Device, TextureUse::RenderTarget};
        const auto expected = samples == 2 || samples == 8 ? KnownGpuIssue::SharedMsaa : KnownGpuIssue::None;
        EXPECT_EQ(FindExternalTextureKnownIssue(dx, dx, native, {}, ExternalTextureTestUse::RenderTarget, supported, supported), expected);
        native.Usage |= copy | TextureUse::Resource;
        EXPECT_EQ(FindExternalTextureKnownIssue(dx, dx, native, {}, ExternalTextureTestUse::RenderTarget, supported, supported), expected);
        native.Format = TextureFormat::RGBA16_FLOAT;
        EXPECT_EQ(FindExternalTextureKnownIssue(dx, dx, native, {}, ExternalTextureTestUse::RenderTarget, supported, supported), KnownGpuIssue::None);
    }
}

TEST(KnownGpuIssueContract, OnlyRecordedDepthResolveModeAndRegionMatch) {
    GpuTestEnvironment environment{RenderBackend::D3D12, 0x10de, 0x2704, 0x0020000000100835ull};
    TextureDescriptor source{TextureDimension::Dim2D, 8, 8, 1, 1, 4, TextureFormat::D32_FLOAT_S8_UINT, MemoryType::Device, TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite | TextureUse::Resource};
    auto target = source;
    target.SampleCount = 1;
    const Rect area{2, 2, 4, 4};
    for (const auto depth : {ResolveMode::None, ResolveMode::SampleZero, ResolveMode::Average, ResolveMode::Min, ResolveMode::Max})
        for (const auto stencil : {ResolveMode::None, ResolveMode::SampleZero, ResolveMode::Min, ResolveMode::Max})
            EXPECT_EQ(FindDepthResolveKnownIssue(environment, source, target, area, depth, stencil), depth == ResolveMode::Average && stencil == ResolveMode::None ? KnownGpuIssue::DepthAverageResolve : KnownGpuIssue::None);
    EXPECT_EQ(FindDepthResolveKnownIssue(environment, source, target, {0, 0, 8, 8}, ResolveMode::Average, ResolveMode::None), KnownGpuIssue::None);
    ++target.Width;
    EXPECT_EQ(FindDepthResolveKnownIssue(environment, source, target, area, ResolveMode::Average, ResolveMode::None), KnownGpuIssue::None);
    --target.Width;
    ++environment.DriverVersion;
    EXPECT_EQ(FindDepthResolveKnownIssue(environment, source, target, area, ResolveMode::Average, ResolveMode::None), KnownGpuIssue::None);
}
}  // namespace
}  // namespace radray::render::test
