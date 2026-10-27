#pragma once

#include "external_resource_test_utils.h"

namespace radray::render::test {

struct GpuTestEnvironment {
    RenderBackend Backend{RenderBackend::D3D12};
    uint32_t VendorId{0}, DeviceId{0};
    uint64_t DriverVersion{0};
};

enum class KnownGpuIssue { None,
                           PartialMipArray,
                           SharedMsaa,
                           CopyOnlyD32S8,
                           DepthAverageResolve };

ExternalResult<GpuTestEnvironment> GetGpuTestEnvironment(const DeviceContext& context);
KnownGpuIssue FindExternalTextureKnownIssue(const GpuTestEnvironment& source, const GpuTestEnvironment& target,
                                            const TextureDescriptor& descriptor, const ExternalTextureDescriptor& options,
                                            ExternalTextureTestUse use, const ExternalSupport& sourceSupport, const ExternalSupport& targetSupport,
                                            bool localControl = false);
KnownGpuIssue FindDepthResolveKnownIssue(const GpuTestEnvironment& environment, const TextureDescriptor& source,
                                         const TextureDescriptor& target, const Rect& area, ResolveMode depth, ResolveMode stencil);
std::string_view KnownGpuIssueName(KnownGpuIssue issue);
bool ShouldSkipKnownGpuIssue(KnownGpuIssue issue, bool runKnownIssues);

// Fixture-owned accounting; aggregate tests call GTEST_SKIP only after checking for failures.
class KnownGpuIssueSkips {
public:
    bool Add(KnownGpuIssue issue, std::string_view combination);
    size_t Count() const;
    string Summary() const;

private:
    vector<string> _entries;
};

}  // namespace radray::render::test
