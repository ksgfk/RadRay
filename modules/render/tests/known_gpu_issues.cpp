#include "known_gpu_issues.h"

namespace radray::render::test {
namespace {
bool VerifiedEnvironment(const GpuTestEnvironment& environment) {
    if (environment.VendorId != 0x10de || environment.DeviceId != 0x2704) return false;
    if (environment.Backend == RenderBackend::D3D12) return environment.DriverVersion == 0x0020000000100835ull;
    return environment.Backend == RenderBackend::Vulkan && environment.DriverVersion == 0x9b404000;
}
bool Shape(const TextureDescriptor& d, TextureDimension dim, uint32_t width, uint32_t height, uint32_t layers, uint32_t mips, uint32_t samples) {
    return d.Dim == dim && d.Width == width && d.Height == height && d.DepthOrArraySize == layers && d.MipLevels == mips && d.SampleCount == samples;
}
bool RecordedColorFormat(TextureFormat format) {
    const TextureFormat formats[]{
        TextureFormat::R8_SINT, TextureFormat::R8_UINT, TextureFormat::R8_SNORM, TextureFormat::R8_UNORM,
        TextureFormat::R16_SINT, TextureFormat::R16_UINT, TextureFormat::R16_SNORM, TextureFormat::R16_UNORM, TextureFormat::R16_FLOAT,
        TextureFormat::RG8_SINT, TextureFormat::RG8_UINT, TextureFormat::RG8_SNORM, TextureFormat::RG8_UNORM,
        TextureFormat::R32_SINT, TextureFormat::R32_UINT, TextureFormat::R32_FLOAT,
        TextureFormat::RG16_SINT, TextureFormat::RG16_UINT, TextureFormat::RG16_SNORM, TextureFormat::RG16_UNORM, TextureFormat::RG16_FLOAT,
        TextureFormat::RGBA8_SINT, TextureFormat::RGBA8_UINT, TextureFormat::RGBA8_SNORM, TextureFormat::RGBA8_UNORM, TextureFormat::RGBA8_UNORM_SRGB,
        TextureFormat::BGRA8_UNORM, TextureFormat::BGRA8_UNORM_SRGB, TextureFormat::RGB10A2_UINT, TextureFormat::RGB10A2_UNORM, TextureFormat::RG11B10_FLOAT,
        TextureFormat::RG32_SINT, TextureFormat::RG32_UINT, TextureFormat::RG32_FLOAT,
        TextureFormat::RGBA16_SINT, TextureFormat::RGBA16_UINT, TextureFormat::RGBA16_SNORM, TextureFormat::RGBA16_UNORM, TextureFormat::RGBA16_FLOAT,
        TextureFormat::RGBA32_SINT, TextureFormat::RGBA32_UINT, TextureFormat::RGBA32_FLOAT};
    return std::ranges::find(formats, format) != std::end(formats);
}
bool RecordedDepthFormat(TextureFormat format) {
    return format == TextureFormat::D16_UNORM || format == TextureFormat::D32_FLOAT || format == TextureFormat::D24_UNORM_S8_UINT || format == TextureFormat::D32_FLOAT_S8_UINT;
}
TextureUses UseFlags(ExternalTextureTestUse use, bool depth) {
    switch (use) {
        case ExternalTextureTestUse::Copy: return {};
        case ExternalTextureTestUse::Sampled: return TextureUse::Resource;
        case ExternalTextureTestUse::Storage: return TextureUse::UnorderedAccess;
        case ExternalTextureTestUse::RenderTarget: return depth ? TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite : TextureUses{TextureUse::RenderTarget};
    }
    return {};
}
}  // namespace

ExternalResult<GpuTestEnvironment> GetGpuTestEnvironment(const DeviceContext& context) {
    GpuTestEnvironment result;
    result.Backend = context.Device->GetBackend();
#if defined(RADRAY_ENABLE_D3D12)
    if (result.Backend == RenderBackend::D3D12) {
        auto* device = static_cast<d3d12::DeviceD3D12*>(context.NativeDevice.Get());
        DXGI_ADAPTER_DESC1 adapter{};
        const auto description = device->_dxgiAdapter->GetDesc1(&adapter);
        if (FAILED(description)) return ExternalError{"GetDesc1 for test environment failed", ExternalNativeApi::D3D12, description};
        LARGE_INTEGER driver{};
        const auto version = device->_dxgiAdapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver);
        if (FAILED(version)) return ExternalError{"CheckInterfaceSupport for test driver version failed", ExternalNativeApi::D3D12, version};
        result.VendorId = adapter.VendorId;
        result.DeviceId = adapter.DeviceId;
        result.DriverVersion = uint64_t(driver.QuadPart);
    }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (result.Backend == RenderBackend::Vulkan) {
        const auto& properties = static_cast<vulkan::DeviceVulkan*>(context.NativeDevice.Get())->_properties;
        result.VendorId = properties.vendorID;
        result.DeviceId = properties.deviceID;
        result.DriverVersion = properties.driverVersion;
    }
#endif
    RADRAY_INFO_LOG("Known-issue environment backend={} vendor={:#x} device={:#x} driver={:#x}", result.Backend, result.VendorId, result.DeviceId, result.DriverVersion);
    return result;
}

KnownGpuIssue FindExternalTextureKnownIssue(const GpuTestEnvironment& source, const GpuTestEnvironment& target,
                                            const TextureDescriptor& d, const ExternalTextureDescriptor& options,
                                            ExternalTextureTestUse use, const ExternalSupport& sourceSupport, const ExternalSupport& targetSupport, bool localControl) {
    if (!ExternalSupportQuerySucceeded(sourceSupport) || !ExternalSupportQuerySucceeded(targetSupport) ||
        !sourceSupport.CanCreateExportable || !targetSupport.CanImport || localControl ||
        !VerifiedEnvironment(source) || !VerifiedEnvironment(target) || source.Backend != RenderBackend::D3D12 ||
        options.HandleType != ExternalMemoryHandleType::D3D12Resource || d.Memory != MemoryType::Device ||
        (d.Hints && d.Hints != ResourceHint::Dedicated)) return KnownGpuIssue::None;
    const auto copy = TextureUse::CopySource | TextureUse::CopyDestination;
    const bool reinterpret = options.AllowFormatReinterpretation;
    if (target.Backend == RenderBackend::Vulkan && d.SampleCount == 1) {
        if (!reinterpret && use == ExternalTextureTestUse::Copy && d.Format == TextureFormat::D32_FLOAT_S8_UINT && d.Usage == copy &&
            Shape(d, TextureDimension::Dim2D, 9, 5, 1, 3, 1)) return KnownGpuIssue::CopyOnlyD32S8;
        if (!reinterpret && use == ExternalTextureTestUse::Storage && d.Usage == (copy | TextureUse::UnorderedAccess) &&
            (d.Format == TextureFormat::RGBA8_UNORM || d.Format == TextureFormat::RGBA16_FLOAT) &&
            (Shape(d, TextureDimension::Dim1DArray, 17, 1, 3, 3, 1) || Shape(d, TextureDimension::Dim2DArray, 17, 9, 3, 3, 1) ||
             Shape(d, TextureDimension::Cube, 17, 17, 6, 3, 1) || Shape(d, TextureDimension::CubeArray, 17, 17, 12, 3, 1)))
            return KnownGpuIssue::PartialMipArray;
        const bool srgbStorage = use == ExternalTextureTestUse::Storage && (d.Format == TextureFormat::RGBA8_UNORM_SRGB || d.Format == TextureFormat::BGRA8_UNORM_SRGB);
        if (!reinterpret && !srgbStorage && RecordedColorFormat(d.Format) && uint32_t(use) <= uint32_t(ExternalTextureTestUse::RenderTarget) &&
            d.Usage == (copy | UseFlags(use, false)) && Shape(d, TextureDimension::Dim2DArray, 9, 5, 2, 2, 1))
            return KnownGpuIssue::PartialMipArray;
        if (RecordedDepthFormat(d.Format) && (use == ExternalTextureTestUse::Copy || use == ExternalTextureTestUse::Sampled || use == ExternalTextureTestUse::RenderTarget) &&
            reinterpret == (use == ExternalTextureTestUse::Sampled) && d.Usage == (copy | TextureUse::DepthStencilWrite | UseFlags(use, true)) &&
            Shape(d, TextureDimension::Dim2DArray, 9, 5, 2, 3, 1)) return KnownGpuIssue::PartialMipArray;
        if (reinterpret && use == ExternalTextureTestUse::Storage && (d.Format == TextureFormat::RGBA8_UNORM || d.Format == TextureFormat::BGRA8_UNORM) &&
            d.Usage == (copy | TextureUse::Resource | TextureUse::RenderTarget | TextureUse::UnorderedAccess) &&
            Shape(d, TextureDimension::Dim2DArray, 9, 5, 2, 3, 1)) return KnownGpuIssue::PartialMipArray;
        // The failing IPC generation has its own exact shape; later generations are not inferred.
        if (!reinterpret && use == ExternalTextureTestUse::Copy && d.Format == TextureFormat::RGBA16_FLOAT && d.Usage == copy &&
            Shape(d, TextureDimension::Dim2DArray, 35, 19, 3, 3, 1)) return KnownGpuIssue::PartialMipArray;
    }
    if (target.Backend == RenderBackend::D3D12 && use == ExternalTextureTestUse::RenderTarget && (d.SampleCount == 2 || d.SampleCount == 8) &&
        (Shape(d, TextureDimension::Dim2D, 8, 8, 1, 1, d.SampleCount) || Shape(d, TextureDimension::Dim2DArray, 8, 8, 2, 1, d.SampleCount))) {
        const bool depth = d.Format == TextureFormat::D32_FLOAT || d.Format == TextureFormat::D24_UNORM_S8_UINT;
        if ((depth || d.Format == TextureFormat::RGBA8_UNORM || d.Format == TextureFormat::RGBA16_FLOAT || d.Format == TextureFormat::R32_UINT) &&
            reinterpret == depth && d.Usage == (TextureUse::Resource | UseFlags(use, depth))) return KnownGpuIssue::SharedMsaa;
        // Native clear/resolve and IPC generation 0 were separately reproduced.
        if (!reinterpret && d.Format == TextureFormat::RGBA8_UNORM && d.Dim == TextureDimension::Dim2D &&
            (d.Usage == TextureUse::RenderTarget || d.Usage == (copy | TextureUse::Resource | TextureUse::RenderTarget))) return KnownGpuIssue::SharedMsaa;
    }
    return KnownGpuIssue::None;
}

KnownGpuIssue FindDepthResolveKnownIssue(const GpuTestEnvironment& environment, const TextureDescriptor& source, const TextureDescriptor& target,
                                         const Rect& area, ResolveMode depth, ResolveMode stencil) {
    const auto usage = TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite | TextureUse::Resource;
    if (VerifiedEnvironment(environment) && environment.Backend == RenderBackend::D3D12 &&
        Shape(source, TextureDimension::Dim2D, 8, 8, 1, 1, 4) && Shape(target, TextureDimension::Dim2D, 8, 8, 1, 1, 1) &&
        source.Format == TextureFormat::D32_FLOAT_S8_UINT && target.Format == source.Format && source.Usage == usage && target.Usage == usage &&
        source.Memory == MemoryType::Device && target.Memory == MemoryType::Device && !source.Hints && !target.Hints &&
        area.X == 2 && area.Y == 2 && area.Width == 4 && area.Height == 4 && depth == ResolveMode::Average && stencil == ResolveMode::None)
        return KnownGpuIssue::DepthAverageResolve;
    return KnownGpuIssue::None;
}

std::string_view KnownGpuIssueName(KnownGpuIssue issue) {
    switch (issue) {
        case KnownGpuIssue::PartialMipArray: return "RTX4080-621.01-DXVK-PARTIAL-MIP: shared array content mismatch; cause unconfirmed";
        case KnownGpuIssue::SharedMsaa: return "RTX4080-621.01-DXDX-MSAA: shared 2x/8x content mismatch; cause unconfirmed";
        case KnownGpuIssue::CopyOnlyD32S8: return "RTX4080-621.01-DXVK-D32S8-COPY: imported stencil read DeviceLost; cause unconfirmed";
        case KnownGpuIssue::DepthAverageResolve: return "RTX4080-621.01-DEPTH-AVERAGE: untouched stencil changes; cause unconfirmed";
        case KnownGpuIssue::None: return "";
    }
    return "";
}
bool ShouldSkipKnownGpuIssue(KnownGpuIssue issue, bool runKnownIssues) { return issue != KnownGpuIssue::None && !runKnownIssues; }
bool KnownGpuIssueSkips::Add(KnownGpuIssue issue, std::string_view combination) {
    const auto* setting = std::getenv("RADRAY_TEST_RUN_KNOWN_GPU_ISSUES");
    if (!ShouldSkipKnownGpuIssue(issue, setting && std::string_view{setting} == "1")) return false;
    _entries.push_back(fmt::format("{} [{}]", KnownGpuIssueName(issue), combination));
    fmt::print("KNOWN_ISSUE_SKIP {}\n", _entries.back());
    testing::Test::RecordProperty("known_issue_skip_count", int(_entries.size()));
    testing::Test::RecordProperty("known_issue_skips", Summary());
    return true;
}
size_t KnownGpuIssueSkips::Count() const { return _entries.size(); }
string KnownGpuIssueSkips::Summary() const { return fmt::format("{}", fmt::join(_entries, "; ")); }

}  // namespace radray::render::test
