#include "test_external_texture.h"
#include "known_gpu_issues.h"
#include <radray/scope_guard.h>

namespace radray::render {
void PrintTo(const ExternalTextureParameter& value, std::ostream* output) { *output << value.Case.Name << " route=" << value.Route << " local=" << value.LocalControl; }
int ExternalResourceTextureTest::Route() const { return GetParam().Route; }
void ExternalResourceTextureTest::SetUp() {
    Case = GetParam().Case;
    ExternalResourceFixture::SetUp();
    if (IsSkipped() || HasFatalFailure()) return;
    if (Target.Get() == &Source && !GetParam().LocalControl) {
        shared_ptr<DXGIDiagnosticContext> diagnostics;
#if defined(RADRAY_ENABLE_D3D12)
        diagnostics = Source.DiagnosticContext;
#endif
        if (!test::TryCreateDevice(Consumer(), Destination, true, true, {1, 0, 0}, diagnostics, true, true, Source.VulkanInstance, true)) {
            if (test::SetupMustFail(Destination.Status, test::RequiredBackend(Consumer()))) FAIL() << Destination.Reason;
            GTEST_SKIP() << Destination.Reason;
        }
        Target = &Destination;
        Importer = Target->Device->GetExternalResourceExtension().Get();
    }
    const ExternalTextureDescriptor options{MemoryDescriptor().HandleType, Case.Reinterpret};
    const auto source = test::QueryExternalTextureSupport(Source, Case.Descriptor, options, "producer");
    const auto target = test::QueryExternalTextureSupport(*Target, Case.Descriptor, options, "consumer");
    RADRAY_INFO_LOG("External texture route={} case={} source='{}' driver='{}' target='{}' driver='{}' extent={}x{}x{} mips={} samples={} reinterpret={} export={} import={} sourceReason='{}' targetReason='{}'",
                    Route(), Case.Name, Source.AdapterName, Source.DriverVersion, Target->AdapterName, Target->DriverVersion,
                    Case.Descriptor.Width, Case.Descriptor.Height, Case.Descriptor.DepthOrArraySize, Case.Descriptor.MipLevels, Case.Descriptor.SampleCount, Case.Reinterpret,
                    source.CanCreateExportable, target.CanImport, source.Reason.Message, target.Reason.Message);
    RecordProperty("source_driver", Source.DriverVersion);
    RecordProperty("target_driver", Target->DriverVersion);
    RecordProperty("route", Route());
    RecordProperty("texture", Case.Name);
    RecordProperty("producer_support", source.Reason.Message);
    RecordProperty("producer_exportable", source.CanCreateExportable ? "true" : "false");
    RecordProperty("consumer_importable", target.CanImport ? "true" : "false");
#if defined(RADRAY_ENABLE_VULKAN)
    if (Target->Device->GetBackend() == RenderBackend::Vulkan)
        RecordProperty("vulkan_driver_version_raw", fmt::format("{}", static_cast<vulkan::DeviceVulkan*>(Target->NativeDevice.Get())->_properties.driverVersion));
#endif
    RecordProperty("consumer_support", target.Reason.Message);
    RecordProperty("producer_native_code", fmt::format("{}", source.Reason.NativeCode));
    RecordProperty("consumer_native_code", fmt::format("{}", target.Reason.NativeCode));
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(source));
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(target));
    const auto sourceFence = Exporter->QueryFenceSupport(FenceType());
    const auto targetFence = Importer->QueryFenceSupport(FenceType());
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(sourceFence));
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(targetFence));
    if (!source.CanCreateExportable || !target.CanImport) GTEST_SKIP() << source.Reason.Message << "; " << target.Reason.Message;
    if (!sourceFence.CanCreateExportable || !targetFence.CanImport)
        GTEST_SKIP() << "external timeline fence route unsupported";
    if (Case.Use == test::ExternalTextureTestUse::Storage &&
        ((Producer() == RenderBackend::Vulkan && !Source.VulkanStorageImageWriteWithoutFormatEnabled) ||
         (Consumer() == RenderBackend::Vulkan && !Target->VulkanStorageImageWriteWithoutFormatEnabled)))
        GTEST_SKIP() << "shaderStorageImageWriteWithoutFormat not enabled";
    test::GpuTestEnvironment sourceEnvironment, targetEnvironment;
    ASSERT_TRUE(test::TakeExternal(test::GetGpuTestEnvironment(Source), sourceEnvironment));
    ASSERT_TRUE(test::TakeExternal(test::GetGpuTestEnvironment(*Target), targetEnvironment));
    test::KnownGpuIssueSkips skips;
    if (skips.Add(test::FindExternalTextureKnownIssue(sourceEnvironment, targetEnvironment, Case.Descriptor, options, Case.Use, source, target, GetParam().LocalControl), Case.Name))
        GTEST_SKIP() << skips.Summary();
}

TEST_P(ExternalResourceTextureTest, MetadataAndFourRoundTrips) {
    ExternalErrorLogProbe errors;
    unique_ptr<Texture> local, remote;
    const ExternalTextureDescriptor options{MemoryDescriptor().HandleType, Case.Reinterpret};
    ASSERT_TRUE(test::TakeExternal(Exporter->CreateExportableTexture(Case.Descriptor, options), local));
    test::ExpectTextureDescriptor(local->GetDesc(), Case.Descriptor);
#if defined(RADRAY_ENABLE_D3D12)
    if (Producer() == RenderBackend::D3D12 && Consumer() == RenderBackend::D3D12) {
        d3d12::ComPtr<IUnknown> sourceIdentity, targetIdentity;
        ASSERT_TRUE(SUCCEEDED(static_cast<d3d12::DeviceD3D12*>(Source.NativeDevice.Get())->_device.As(&sourceIdentity)));
        ASSERT_TRUE(SUCCEEDED(static_cast<d3d12::DeviceD3D12*>(Target->NativeDevice.Get())->_device.As(&targetIdentity)));
        RecordProperty("same_native_device", sourceIdentity.Get() == targetIdentity.Get() ? "true" : "false");
    }
#endif
    ExternalTextureExport exported;
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportTexture(local.get()), exported));
    EXPECT_EQ(exported.Info.AllowFormatReinterpretation, Case.Reinterpret);
    EXPECT_EQ(exported.Info.SourceBackend, Producer());
    test::ExpectTextureDescriptor(exported.Info.Descriptor, Case.Descriptor);
#if defined(RADRAY_ENABLE_D3D12)
    if (Producer() == RenderBackend::D3D12) {
        const auto native = d3d12::MakeTextureResourceDesc(Case.Descriptor, Case.Reinterpret);
        EXPECT_EQ(exported.Info.NativeFormat, uint32_t(native.Format));
        EXPECT_EQ(exported.Info.NativeFlags, uint32_t(native.Flags));
    }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (Producer() == RenderBackend::Vulkan) {
        const auto native = vulkan::MakeTextureImageCreateInfo(Case.Descriptor, Case.Reinterpret);
        EXPECT_EQ(exported.Info.NativeFormat, uint32_t(native.format));
        EXPECT_EQ(exported.Info.NativeFlags, native.flags);
        EXPECT_EQ(exported.Info.NativeUsage, native.usage);
        EXPECT_EQ(exported.Info.NativeTiling, uint32_t(native.tiling));
    }
#endif
    ASSERT_TRUE(test::TakeExternal(Importer->ImportTexture(exported.Handle.Borrow(), exported.Info), remote));
    exported.Handle.Reset();
    test::ExpectTextureDescriptor(remote->GetDesc(), Case.Descriptor);
    if (Case.Descriptor.Usage.HasFlag(TextureUse::Resource)) {
        for (const bool destination : {false, true}) {
            auto& context = destination ? *Target : Source;
            auto* texture = destination ? remote.get() : local.get();
            const auto& d = Case.Descriptor;
            const SubresourceRange range{0, d.Dim == TextureDimension::Dim3D ? 1u : d.DepthOrArraySize, 0, d.MipLevels, IsDepthStencilFormat(d.Format) ? TextureAspect::Depth : TextureAspect::Color};
            ASSERT_TRUE(context.Device->CreateTextureView({texture, d.Dim, d.Format, range, TextureViewUsage::Resource}));
            if (!Case.Reinterpret && (d.Format == TextureFormat::RGBA8_UNORM || d.Format == TextureFormat::BGRA8_UNORM)) {
                const auto before = context.RhiValidationErrors.load();
                const auto srgb = d.Format == TextureFormat::RGBA8_UNORM ? TextureFormat::RGBA8_UNORM_SRGB : TextureFormat::BGRA8_UNORM_SRGB;
                EXPECT_FALSE(context.Device->CreateTextureView({texture, d.Dim, srgb, range, TextureViewUsage::Resource}));
                EXPECT_EQ(context.RhiValidationErrors.load(), before + 1);
                context.RhiValidationErrors = before;
            }
        }
    }
    if (Case.Reinterpret && !IsDepthStencilFormat(Case.Descriptor.Format)) {
        for (const bool destination : {false, true}) {
            auto& context = destination ? *Target : Source;
            auto* texture = destination ? remote.get() : local.get();
            const auto srgb = Case.Descriptor.Format == TextureFormat::RGBA8_UNORM ? TextureFormat::RGBA8_UNORM_SRGB : TextureFormat::BGRA8_UNORM_SRGB;
            ASSERT_TRUE(context.Device->CreateTextureView({texture, TextureDimension::Dim2DArray, srgb, {0, 1, 0, 1}, TextureViewUsage::RenderTarget}));
            if (Case.Descriptor.Format == TextureFormat::RGBA8_UNORM) {
                const auto before = context.RhiValidationErrors.load();
                auto different = context.Device->CreateTextureView({texture, TextureDimension::Dim2DArray, TextureFormat::R32_UINT, {0, 1, 0, 1}, TextureViewUsage::Resource});
                EXPECT_EQ(bool(different), context.Device->GetBackend() == RenderBackend::Vulkan);
                if (context.Device->GetBackend() == RenderBackend::D3D12) {
                    EXPECT_EQ(context.RhiValidationErrors.load(), before + 1);
                    context.RhiValidationErrors = before;
                }
            }
        }
    }
    unique_ptr<Fence> ready, released, readyImported, releasedImported;
    ASSERT_TRUE(test::TakeExternal(Exporter->CreateExportableFence({FenceType()}), ready));
    ASSERT_TRUE(test::TakeExternal(Exporter->CreateExportableFence({FenceType()}), released));
    ExternalFenceExport readyHandle, releasedHandle;
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportFence(ready.get()), readyHandle));
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportFence(released.get()), releasedHandle));
    ASSERT_TRUE(test::TakeExternal(Importer->ImportFence(readyHandle.Handle.Borrow(), readyHandle.Info), readyImported));
    ASSERT_TRUE(test::TakeExternal(Importer->ImportFence(releasedHandle.Handle.Borrow(), releasedHandle.Info), releasedImported));
    readyHandle.Handle.Reset();
    releasedHandle.Handle.Reset();
    ASSERT_TRUE(test::ExternalTextureTurn(Source, local.get(), Case.Use, 0, 1, true, released.get(), 0, ready.get(), 1, nullptr, Case.Reinterpret));
    for (uint32_t frame = 1; frame <= 4; ++frame) {
        ASSERT_TRUE(test::ExternalTextureTurn(*Target, GetParam().LocalControl ? local.get() : remote.get(), Case.Use, frame * 2 - 1, frame * 2, false, readyImported.get(), frame, releasedImported.get(), frame, nullptr, Case.Reinterpret));
        ASSERT_TRUE(test::ExternalTextureTurn(Source, local.get(), Case.Use, frame * 2, frame * 2 + 1, false, released.get(), frame, ready.get(), frame + 1, nullptr, Case.Reinterpret));
    }
    EXPECT_TRUE(errors.Take().empty());
}

TEST_P(ExternalResourceConfigurationTest, QueryEveryTextureCaseWithoutCreatingResources) {
    test::DeviceContext context;
    if (!test::TryCreateDevice(GetParam(), context, true, true, {1, 0, 0}, nullptr, true, true, nullptr, true)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(GetParam()))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    ASSERT_TRUE(context.Device->GetExternalResourceExtension());
    for (const auto& value : test::ExternalTextureCases()) {
        for (const auto handle : {ExternalMemoryHandleType::D3D12Resource, ExternalMemoryHandleType::OpaqueWin32}) {
            if (GetParam() == RenderBackend::D3D12 && handle == ExternalMemoryHandleType::OpaqueWin32) continue;
            SCOPED_TRACE(fmt::format("{} handle={}", value.Name, uint32_t(handle)));
            const auto support = test::QueryExternalTextureSupport(context, value.Descriptor, {handle, value.Reinterpret}, value.Name);
            EXPECT_TRUE(test::ExternalSupportQuerySucceeded(support));
        }
    }
    context.Reset();
}

TEST_P(ExternalResourceConfigurationTest, TypedDepthSamplingRequiresExplicitReinterpretationOnD3D12) {
    test::DeviceContext context;
    if (!test::TryCreateDevice(GetParam(), context, true, true, {1, 0, 0}, nullptr, true, true, nullptr, true)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(GetParam()))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    const auto extension = context.Device->GetExternalResourceExtension();
    if (!extension) GTEST_SKIP() << "external resource extension unavailable";
    ExternalErrorLogProbe errors;
    const ExternalTextureDescriptor options{GetParam() == RenderBackend::Vulkan ? ExternalMemoryHandleType::OpaqueWin32 : ExternalMemoryHandleType::D3D12Resource};
    for (auto format : {TextureFormat::D16_UNORM, TextureFormat::D32_FLOAT, TextureFormat::D24_UNORM_S8_UINT, TextureFormat::D32_FLOAT_S8_UINT}) {
        const TextureDescriptor d{TextureDimension::Dim2D, 8, 8, 1, 1, 1, format, MemoryType::Device, TextureUse::Resource | TextureUse::DepthStencilWrite};
        const auto support = extension->QueryTextureSupport(d, options);
        ASSERT_TRUE(test::ExternalSupportQuerySucceeded(support));
        if (GetParam() == RenderBackend::D3D12) {
            EXPECT_FALSE(support.CanCreateExportable);
            EXPECT_FALSE(support.CanImport);
            EXPECT_NE(support.Reason.Message.find("AllowFormatReinterpretation"), string::npos);
        }
    }
    EXPECT_TRUE(errors.Take().empty());
}

TEST_P(ExternalResourceConfigurationTest, NativeDenyShaderResourceAllowsAttachmentImport) {
    if (GetParam() != RenderBackend::D3D12) GTEST_SKIP() << "D3D12 native resource flag contract";
#if defined(RADRAY_ENABLE_D3D12)
    test::DeviceContext context;
    if (!test::TryCreateDevice(GetParam(), context, true, true, {1, 0, 0}, nullptr, true, true, nullptr, true)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(GetParam()))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    auto extension = context.Device->GetExternalResourceExtension().Get();
    const TextureDescriptor descriptor{TextureDimension::Dim2D, 8, 8, 1, 1, 1, TextureFormat::D32_FLOAT, MemoryType::Device, TextureUse::DepthStencilWrite | TextureUse::CopySource};
    const ExternalTextureDescriptor options{ExternalMemoryHandleType::D3D12Resource, true};
    const auto support = test::QueryExternalTextureSupport(context, descriptor, options, "deny-shader-resource");
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(support));
    if (!support.CanImport) GTEST_SKIP() << "depth format unavailable";
    auto* device = static_cast<d3d12::DeviceD3D12*>(context.NativeDevice.Get());
    auto native = d3d12::MakeTextureResourceDesc(descriptor, true);
    native.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    d3d12::ComPtr<ID3D12Resource> resource;
    ASSERT_TRUE(SUCCEEDED(device->_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &native, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))));
    HANDLE raw = nullptr;
    ASSERT_TRUE(SUCCEEDED(device->_device->CreateSharedHandle(resource.Get(), nullptr, GENERIC_ALL, nullptr, &raw)));
    OwnedExternalHandle handle{reinterpret_cast<uintptr_t>(raw)};
    ExternalTextureInfo info;
    const auto identity = extension->GetDeviceIdentity();
    ASSERT_TRUE(std::holds_alternative<ExternalDeviceIdentity>(identity));
    info.Device = std::get<ExternalDeviceIdentity>(identity);
    info.Descriptor = descriptor;
    info.NativeFormat = uint32_t(native.Format);
    info.NativeFlags = uint32_t(native.Flags);
    info.NativeTiling = uint32_t(native.Layout);
    info.Allocation.Size = device->_device->GetResourceAllocationInfo(0, 1, &native).SizeInBytes;
    info.AllowFormatReinterpretation = true;
    ExternalErrorLogProbe errors;
    unique_ptr<Texture> imported;
    ASSERT_TRUE(test::TakeExternal(extension->ImportTexture(handle.Borrow(), info), imported));
    EXPECT_TRUE(errors.Take().empty());
    info.Descriptor.Usage |= TextureUse::Resource;
    const auto rejected = extension->ImportTexture(handle.Borrow(), info);
    EXPECT_TRUE(std::holds_alternative<ExternalError>(rejected));
    EXPECT_EQ(errors.Take().size(), 1u);
#else
    GTEST_SKIP() << "D3D12 backend not built";
#endif
}

namespace {
TEST(ExternalResourceNativeProbe, VulkanAttachmentDimensionsRespectFramebufferLimits) {
#if defined(RADRAY_ENABLE_VULKAN)
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::Vulkan, context, true, true, {1, 0, 0}, nullptr, true, true, nullptr, true)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(RenderBackend::Vulkan))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    auto* device = static_cast<vulkan::DeviceVulkan*>(context.NativeDevice.Get());
    if (!device->_externalMemoryWin32) GTEST_SKIP() << "Vulkan Win32 external memory unavailable";
    const auto extension = context.Device->GetExternalResourceExtension();
    ASSERT_TRUE(extension);
    ExternalErrorLogProbe errors;
    for (const bool depth : {false, true}) {
        TextureDescriptor desc{TextureDimension::Dim2D, 1, 1, 1, 1, 1,
                               depth ? TextureFormat::D32_FLOAT : TextureFormat::RGBA8_UNORM, MemoryType::Device,
                               depth ? TextureUse::DepthStencilWrite : TextureUse::RenderTarget};
        for (const bool width : {false, true}) {
            desc.Width = width ? device->_properties.limits.maxFramebufferWidth + 1 : 1;
            desc.Height = width ? 1 : device->_properties.limits.maxFramebufferHeight + 1;
            const auto support = extension->QueryTextureSupport(desc, {ExternalMemoryHandleType::OpaqueWin32});
            EXPECT_FALSE(support.CanCreateExportable);
            EXPECT_FALSE(support.CanImport);
            EXPECT_NE(support.Reason.Message.find("framebuffer limits"), string::npos);
        }
    }
    EXPECT_TRUE(errors.Take().empty());
#else
    GTEST_SKIP() << "Vulkan backend not built";
#endif
}

TEST(ExternalResourceNativeProbe, D3D12ToVulkanMipArray) {
#if defined(RADRAY_ENABLE_D3D12) && defined(RADRAY_ENABLE_VULKAN)
    test::DeviceContext dx, vk;
    if (!test::TryCreateDevice(RenderBackend::D3D12, dx, true, false, {1, 0, 0}, nullptr, true, true, nullptr, true)) {
        if (test::SetupMustFail(dx.Status, test::RequiredBackend(RenderBackend::D3D12))) FAIL() << dx.Reason;
        GTEST_SKIP() << dx.Reason;
    }
    if (!test::TryCreateDevice(RenderBackend::Vulkan, vk, true, false, {1, 0, 0}, nullptr, true, true, nullptr, true)) {
        if (test::SetupMustFail(vk.Status, test::RequiredBackend(RenderBackend::Vulkan))) FAIL() << vk.Reason;
        GTEST_SKIP() << vk.Reason;
    }
    auto* dd = static_cast<d3d12::DeviceD3D12*>(dx.NativeDevice.Get());
    auto* vd = static_cast<vulkan::DeviceVulkan*>(vk.NativeDevice.Get());
    ASSERT_EQ(std::get<ExternalDeviceIdentity>(dd->GetExternalResourceExtension()->GetDeviceIdentity()), std::get<ExternalDeviceIdentity>(vd->GetExternalResourceExtension()->GetDeviceIdentity()));
    auto* dq = static_cast<d3d12::CmdQueueD3D12*>(dd->GetCommandQueue(QueueType::Direct, 0).Get());
    auto* vq = static_cast<vulkan::QueueVulkan*>(vd->GetCommandQueue(QueueType::Direct, 0).Get());
    auto& f = vd->_ftb;
    test::GpuTestEnvironment sourceEnvironment, targetEnvironment;
    ASSERT_TRUE(test::TakeExternal(test::GetGpuTestEnvironment(dx), sourceEnvironment));
    ASSERT_TRUE(test::TakeExternal(test::GetGpuTestEnvironment(vk), targetEnvironment));
    test::KnownGpuIssueSkips skips;
    uint32_t executed = 0;
    for (uint32_t mips : {1u, 2u, 4u}) {
        SCOPED_TRACE(fmt::format("mips={}", mips));
        const VkExternalMemoryHandleTypeFlagBits handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
        const VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, handleType};
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {9, 5, 1}, mips, 2, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
        const VkPhysicalDeviceExternalImageFormatInfo ei{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO, nullptr, handleType};
        const VkPhysicalDeviceImageFormatInfo2 qi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &ei, ci.format, ci.imageType, ci.tiling, ci.usage, ci.flags};
        VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES, nullptr, {}};
        VkImageFormatProperties2 limits{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &ep, {}};
        const auto query = vkGetPhysicalDeviceImageFormatProperties2(vd->_physicalDevice, &qi, &limits);
        fmt::print("native array capability result={} externalFeatures={} usage={}\n", int(query), ep.externalMemoryProperties.externalMemoryFeatures, ci.usage);
        if (query == VK_ERROR_FORMAT_NOT_SUPPORTED) GTEST_SKIP() << "native external format unavailable";
        ASSERT_EQ(query, VK_SUCCESS);
        if (!(ep.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)) GTEST_SKIP() << "native external import unavailable";
        ASSERT_LE(ci.extent.width, limits.imageFormatProperties.maxExtent.width);
        ASSERT_LE(ci.extent.height, limits.imageFormatProperties.maxExtent.height);
        ASSERT_LE(ci.extent.depth, limits.imageFormatProperties.maxExtent.depth);
        ASSERT_LE(ci.mipLevels, limits.imageFormatProperties.maxMipLevels);
        ASSERT_LE(ci.arrayLayers, limits.imageFormatProperties.maxArrayLayers);
        ASSERT_NE(limits.imageFormatProperties.sampleCounts & ci.samples, 0u);
        ASSERT_NE(ep.externalMemoryProperties.compatibleHandleTypes & handleType, 0u);
        const TextureDescriptor logical{TextureDimension::Dim2DArray, 9, 5, 2, mips, 1, TextureFormat::RGBA8_UNORM, MemoryType::Device,
                                        TextureUse::CopySource | TextureUse::CopyDestination | TextureUse::Resource};
        const auto sourceSupport = dd->GetExternalResourceExtension()->QueryTextureSupport(logical);
        const auto targetSupport = vd->GetExternalResourceExtension()->QueryTextureSupport(logical);
        ASSERT_TRUE(test::ExternalSupportQuerySucceeded(sourceSupport));
        ASSERT_TRUE(test::ExternalSupportQuerySucceeded(targetSupport));
        ASSERT_TRUE(sourceSupport.CanCreateExportable);
        ASSERT_TRUE(targetSupport.CanImport);
        if (skips.Add(test::FindExternalTextureKnownIssue(sourceEnvironment, targetEnvironment, logical, {}, test::ExternalTextureTestUse::Sampled, sourceSupport, targetSupport), fmt::format("native array mips={}", mips))) continue;
        ++executed;
        D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 9, 5, 2, uint16_t(mips), DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
        d3d12::ComPtr<ID3D12Resource> source;
        ASSERT_TRUE(SUCCEEDED(dd->_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&source))));
        const uint64_t uploadSize = uint64_t(mips) * 2 * 4096;
        D3D12_RESOURCE_DESC uploadDesc{D3D12_RESOURCE_DIMENSION_BUFFER, 0, uploadSize, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        d3d12::ComPtr<ID3D12Resource> upload;
        ASSERT_TRUE(SUCCEEDED(dd->_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload))));
        void* mapped{};
        const D3D12_RANGE empty{0, 0};
        ASSERT_TRUE(SUCCEEDED(upload->Map(0, &empty, &mapped)));
        d3d12::ComPtr<ID3D12CommandAllocator> allocator;
        d3d12::ComPtr<ID3D12GraphicsCommandList> cmd;
        ASSERT_TRUE(SUCCEEDED(dd->_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
        ASSERT_TRUE(SUCCEEDED(dd->_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&cmd))));
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {source.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST};
        cmd->ResourceBarrier(1, &barrier);
        for (uint32_t layer = 0; layer < 2; ++layer)
            for (uint32_t mip = 0; mip < mips; ++mip) {
                const auto sub = layer * mips + mip;
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
                dd->_device->GetCopyableFootprints(&desc, sub, 1, uint64_t(sub) * 4096, &footprint, nullptr, nullptr, nullptr);
                const uint8_t color[]{uint8_t(layer * 255), uint8_t((mip & 1) * 255), uint8_t(((mip >> 1) & 1) * 255), 255};
                for (uint32_t y = 0; y < footprint.Footprint.Height; ++y)
                    for (uint32_t x = 0; x < footprint.Footprint.Width; ++x)
                        std::memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4, color, 4);
                D3D12_TEXTURE_COPY_LOCATION from{};
                from.pResource = upload.Get();
                from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                from.PlacedFootprint = footprint;
                D3D12_TEXTURE_COPY_LOCATION to{};
                to.pResource = source.Get();
                to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                to.SubresourceIndex = sub;
                cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            }
        upload->Unmap(0, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        cmd->ResourceBarrier(1, &barrier);
        ASSERT_TRUE(SUCCEEDED(cmd->Close()));
        ID3D12CommandList* lists[]{cmd.Get()};
        dq->_queue->ExecuteCommandLists(1, lists);
        dq->Wait();
        HANDLE raw{};
        ASSERT_TRUE(SUCCEEDED(dd->_device->CreateSharedHandle(source.Get(), nullptr, GENERIC_ALL, nullptr, &raw)));
        OwnedExternalHandle handle{reinterpret_cast<uintptr_t>(raw)};
        VkImage image{};
        ASSERT_EQ(f.vkCreateImage(vd->_device, &ci, nullptr, &image), VK_SUCCESS);
        VkDeviceMemory memory{};
        VkCommandPool pool{};
        const auto cleanup = MakeScopeGuard([&] {
            if (pool) f.vkDestroyCommandPool(vd->_device, pool, nullptr);
            f.vkDestroyImage(vd->_device, image, nullptr);
            if (memory) f.vkFreeMemory(vd->_device, memory, nullptr);
        });
        VkMemoryRequirements req{};
        f.vkGetImageMemoryRequirements(vd->_device, image, &req);
        VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR, nullptr, 0};
        ASSERT_EQ(f.vkGetMemoryWin32HandlePropertiesKHR(vd->_device, handleType, raw, &hp), VK_SUCCESS);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(vd->_physicalDevice, &mp);
        uint32_t type = mp.memoryTypeCount;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & hp.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                type = i;
                break;
            }
        ASSERT_LT(type, mp.memoryTypeCount);
        const VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, nullptr, image, VK_NULL_HANDLE};
        const VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated, handleType, raw, nullptr};
        const VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, req.size, type};
        ASSERT_EQ(f.vkAllocateMemory(vd->_device, &allocation, nullptr, &memory), VK_SUCCESS);
        ASSERT_EQ(f.vkBindImageMemory(vd->_device, image, memory, 0), VK_SUCCESS);
        const auto dxSize = dd->_device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        fmt::print("native array mips={} d3dAllocation={} vkRequirements={} memoryType={}\n", mips, dxSize, req.size, type);
        auto readback = vd->CreateBuffer({.Size = uint64_t(mips) * 2 * 512, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead}).Unwrap();
        const VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr, 0, vq->_family.Family};
        ASSERT_EQ(f.vkCreateCommandPool(vd->_device, &pi, nullptr, &pool), VK_SUCCESS);
        const VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        VkCommandBuffer commands{};
        ASSERT_EQ(f.vkAllocateCommandBuffers(vd->_device, &ca, &commands), VK_SUCCESS);
        const VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr, 0, nullptr};
        ASSERT_EQ(f.vkBeginCommandBuffer(commands, &begin), VK_SUCCESS);
        VkImageMemoryBarrier vb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_EXTERNAL, vq->_family.Family, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 2}};
        f.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &vb);
        for (uint32_t layer = 0; layer < 2; ++layer)
            for (uint32_t mip = 0; mip < mips; ++mip) {
                const VkBufferImageCopy copy{uint64_t(layer * mips + mip) * 512, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1}, {0, 0, 0}, {std::max(1u, 9u >> mip), std::max(1u, 5u >> mip), 1}};
                f.vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_GENERAL, static_cast<vulkan::BufferVulkan*>(readback.get())->_buffer, 1, &copy);
            }
        const VkBufferMemoryBarrier hostRead{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, static_cast<vulkan::BufferVulkan*>(readback.get())->_buffer, 0, VK_WHOLE_SIZE};
        f.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &hostRead, 0, nullptr);
        ASSERT_EQ(f.vkEndCommandBuffer(commands), VK_SUCCESS);
        const VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &commands, 0, nullptr};
        ASSERT_EQ(f.vkQueueSubmit(vq->_queue, 1, &submit, VK_NULL_HANDLE), VK_SUCCESS);
        ASSERT_EQ(f.vkQueueWaitIdle(vq->_queue), VK_SUCCESS);
        const auto size = readback->GetDesc().Size;
        const auto* bytes = static_cast<const uint8_t*>(readback->Map(0, size));
        ASSERT_NE(bytes, nullptr);
        readback->InvalidateMappedRange({0, size});
        for (uint32_t layer = 0; layer < 2; ++layer)
            for (uint32_t mip = 0; mip < mips; ++mip) {
                const uint8_t expected[]{uint8_t(layer * 255), uint8_t((mip & 1) * 255), uint8_t(((mip >> 1) & 1) * 255), 255};
                const auto* data = bytes + (layer * mips + mip) * 512;
                bool equal = true;
                for (uint32_t pixel = 0; pixel < std::max(1u, 9u >> mip) * std::max(1u, 5u >> mip); ++pixel)
                    equal &= std::memcmp(data + pixel * 4, expected, 4) == 0;
                fmt::print("native array mips={} layer={} mip={} firstPixel={:08x} equal={}\n", mips, layer, mip, *reinterpret_cast<const uint32_t*>(data), equal);
                EXPECT_TRUE(equal) << fmt::format("layer={} mip={}", layer, mip);
            }
        readback->Unmap();
    }
    RecordProperty("executed_mip_combinations", executed);
    if (skips.Count() && !HasFailure()) GTEST_SKIP() << skips.Summary();
#else
    GTEST_SKIP() << "both backends required";
#endif
}

TEST(ExternalResourceNativeProbe, D3D12SharedMsaaClearResolve) {
#if defined(RADRAY_ENABLE_D3D12)
    test::DeviceContext context;
    if (!test::TryCreateDevice(RenderBackend::D3D12, context, true, true, {1, 0, 0}, nullptr, true, true, nullptr, true)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(RenderBackend::D3D12))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    auto* nativeDevice = static_cast<d3d12::DeviceD3D12*>(context.NativeDevice.Get());
    auto* device = nativeDevice->_device.Get();
    auto* queue = static_cast<d3d12::CmdQueueD3D12*>(nativeDevice->GetCommandQueue(QueueType::Direct, 0).Get());
    uint32_t supportedCombinations = 0;
    uint32_t executedCombinations = 0;
    test::GpuTestEnvironment environment;
    ASSERT_TRUE(test::TakeExternal(test::GetGpuTestEnvironment(context), environment));
    test::KnownGpuIssueSkips skips;
    for (const bool explicitAliasing : {false, true}) {
        for (uint32_t samples : {2u, 4u, 8u}) {
            SCOPED_TRACE(fmt::format("samples={} explicitAliasing={}", samples, explicitAliasing));
            D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS support{DXGI_FORMAT_R8G8B8A8_UNORM, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE, 0};
            const auto result = device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &support, sizeof(support));
            fmt::print("native MSAA query samples={} explicitAliasing={} result={} qualityLevels={}\n", samples, explicitAliasing, int64_t(result), support.NumQualityLevels);
            ASSERT_TRUE(SUCCEEDED(result));
            if (!support.NumQualityLevels) continue;
            ++supportedCombinations;
            const TextureDescriptor logical{TextureDimension::Dim2D, 8, 8, 1, 1, samples, TextureFormat::RGBA8_UNORM, MemoryType::Device, TextureUse::RenderTarget};
            const auto externalSupport = nativeDevice->GetExternalResourceExtension()->QueryTextureSupport(logical);
            ASSERT_TRUE(test::ExternalSupportQuerySucceeded(externalSupport));
            ASSERT_TRUE(externalSupport.CanCreateExportable && externalSupport.CanImport);
            if (skips.Add(test::FindExternalTextureKnownIssue(environment, environment, logical, {}, test::ExternalTextureTestUse::RenderTarget, externalSupport, externalSupport), fmt::format("native MSAA samples={} explicitAliasing={}", samples, explicitAliasing))) continue;
            ++executedCombinations;
            D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 8, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {samples, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 1, 1};
            d3d12::ComPtr<ID3D12Resource> original, imported, resolved, readback;
            ASSERT_TRUE(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&original))));
            HANDLE raw{};
            ASSERT_TRUE(SUCCEEDED(device->CreateSharedHandle(original.Get(), nullptr, GENERIC_ALL, nullptr, &raw)));
            OwnedExternalHandle handle{reinterpret_cast<uintptr_t>(raw)};
            ASSERT_TRUE(SUCCEEDED(device->OpenSharedHandle(raw, IID_PPV_ARGS(&imported))));
            desc.SampleDesc.Count = 1;
            ASSERT_TRUE(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr, IID_PPV_ARGS(&resolved))));
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
            uint64_t size{};
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &size);
            D3D12_RESOURCE_DESC buffer{D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            ASSERT_TRUE(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))));
            d3d12::ComPtr<ID3D12DescriptorHeap> views;
            const D3D12_DESCRIPTOR_HEAP_DESC viewHeap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            ASSERT_TRUE(SUCCEEDED(device->CreateDescriptorHeap(&viewHeap, IID_PPV_ARGS(&views))));
            auto originalRtv = views->GetCPUDescriptorHandleForHeapStart();
            auto importedRtv = originalRtv;
            importedRtv.ptr += device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            device->CreateRenderTargetView(original.Get(), nullptr, originalRtv);
            device->CreateRenderTargetView(imported.Get(), nullptr, importedRtv);
            d3d12::ComPtr<ID3D12CommandAllocator> allocator;
            d3d12::ComPtr<ID3D12GraphicsCommandList> command;
            ASSERT_TRUE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))));
            ASSERT_TRUE(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&command))));
            ASSERT_TRUE(SUCCEEDED(command->Close()));
            const auto transition = [&](ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
                command->ResourceBarrier(1, &barrier);
            };
            const auto submit = [&] {
                if (FAILED(command->Close())) return false;
                ID3D12CommandList* lists[]{command.Get()};
                queue->_queue->ExecuteCommandLists(1, lists);
                queue->Wait();
                return SUCCEEDED(allocator->Reset());
            };
            for (uint32_t turn = 0; turn < 4; ++turn) {
                SCOPED_TRACE(fmt::format("turn={}", turn));
                auto* writer = turn % 2 ? imported.Get() : original.Get();
                auto* reader = turn % 2 ? original.Get() : imported.Get();
                const float color[]{float(turn % 2), float((turn + 1) % 2), 0, 1};
                ASSERT_TRUE(SUCCEEDED(command->Reset(allocator.Get(), nullptr)));
                transition(writer, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
                command->ClearRenderTargetView(turn % 2 ? importedRtv : originalRtv, color, 0, nullptr);
                transition(writer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
                ASSERT_TRUE(submit());
                ASSERT_TRUE(SUCCEEDED(command->Reset(allocator.Get(), nullptr)));
                if (explicitAliasing) {
                    D3D12_RESOURCE_BARRIER aliasing{};
                    aliasing.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                    aliasing.Aliasing = {nullptr, nullptr};
                    command->ResourceBarrier(1, &aliasing);
                }
                transition(reader, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
                command->ResolveSubresource(resolved.Get(), 0, reader, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
                transition(reader, D3D12_RESOURCE_STATE_RESOLVE_SOURCE, D3D12_RESOURCE_STATE_COMMON);
                transition(resolved.Get(), D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION source{};
                source.pResource = resolved.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION destination{};
                destination.pResource = readback.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                destination.PlacedFootprint = footprint;
                command->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
                transition(resolved.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
                ASSERT_TRUE(submit());
                const D3D12_RANGE readRange{0, SIZE_T(size)}, empty{0, 0};
                void* data{};
                ASSERT_TRUE(SUCCEEDED(readback->Map(0, &readRange, &data)));
                bool equal = true;
                uint32_t mismatches = 0;
                for (uint32_t y = 0; y < 8; ++y)
                    for (uint32_t x = 0; x < 8; ++x)
                        for (uint32_t c = 0; c < 4; ++c) {
                            const auto actual = static_cast<const uint8_t*>(data)[y * footprint.Footprint.RowPitch + x * 4 + c];
                            if (actual != uint8_t(color[c] * 255)) {
                                if (mismatches++ == 0) fmt::print("native first mismatch xy={},{} c={} actual={} expected={}\n", x, y, c, actual, color[c] * 255);
                                equal = false;
                            }
                        }
                fmt::print("native MSAA explicitAliasing={} samples={} turn={} firstPixel={:08x} expectedRG={},{} equal={} mismatched_components={}\n", explicitAliasing, samples, turn, *static_cast<const uint32_t*>(data), color[0], color[1], equal, mismatches);
                readback->Unmap(0, &empty);
                EXPECT_TRUE(equal);
            }
        }
    }
    RecordProperty("supported_sample_barrier_combinations", supportedCombinations);
    RecordProperty("executed_sample_barrier_combinations", executedCombinations);
    if (!supportedCombinations) GTEST_SKIP() << "no supported native MSAA sample counts";
    ASSERT_TRUE(context.Diagnostics->Checkpoint());
    std::lock_guard lock{context.Diagnostics->Mutex};
    for (const auto& message : context.Diagnostics->Messages)
        if (message.Severity == D3D12_MESSAGE_SEVERITY_WARNING) fmt::print("native probe warning id={}: {}\n", message.Id, message.Description);
    if (skips.Count() && !HasFailure()) GTEST_SKIP() << skips.Summary();
#else
    GTEST_SKIP() << "D3D12 backend not built";
#endif
}

vector<ExternalTextureParameter> TextureParameters() {
    vector<ExternalTextureParameter> result;
    for (const auto& item : test::ExternalTextureCases()) {
        for (int route = 0; route < 5; ++route) result.push_back({item, route});
        if (item.Name.starts_with("Msaa_")) result.push_back({item, 0, true});
    }
    return result;
}
INSTANTIATE_TEST_SUITE_P(Capabilities, ExternalResourceTextureTest, testing::ValuesIn(TextureParameters()), [](const testing::TestParamInfo<ExternalTextureParameter>& info) {
    return fmt::format("{}{}_Route{}", info.param.LocalControl ? "LocalControl_" : "", info.param.Case.Name, info.param.Route);
});

TEST(ExternalResourceTextureMapping, NativeDimensionsAndWholeOwnershipRange) {
    for (const auto& item : test::ExternalTextureCases()) {
        const auto& d = item.Descriptor;
        SCOPED_TRACE(item.Name);
#if defined(RADRAY_ENABLE_D3D12)
        const auto dx = d3d12::MakeTextureResourceDesc(d, item.Reinterpret);
        EXPECT_EQ(dx.Width, d.Width);
        EXPECT_EQ(dx.Height, d.Height);
        EXPECT_EQ(dx.DepthOrArraySize, d.DepthOrArraySize);
        EXPECT_EQ(dx.MipLevels, d.MipLevels);
        EXPECT_EQ(dx.SampleDesc.Count, d.SampleCount);
        EXPECT_EQ(dx.SampleDesc.Quality, 0u);
        EXPECT_EQ(dx.Dimension, d.Dim == TextureDimension::Dim3D ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : d.Dim == TextureDimension::Dim1D || d.Dim == TextureDimension::Dim1DArray ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                                                                                                                                                                                  : D3D12_RESOURCE_DIMENSION_TEXTURE2D);
        EXPECT_EQ(bool(dx.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL), d.Usage.HasFlag(TextureUse::DepthStencilWrite) || d.Usage.HasFlag(TextureUse::DepthStencilRead));
#endif
#if defined(RADRAY_ENABLE_VULKAN)
        const auto vk = vulkan::MakeTextureImageCreateInfo(d, item.Reinterpret);
        EXPECT_EQ(vk.extent.width, d.Width);
        EXPECT_EQ(vk.extent.height, d.Height);
        EXPECT_EQ(vk.extent.depth, d.Dim == TextureDimension::Dim3D ? d.DepthOrArraySize : 1u);
        EXPECT_EQ(vk.arrayLayers, d.Dim == TextureDimension::Dim3D ? 1u : d.DepthOrArraySize);
        EXPECT_EQ(vk.mipLevels, d.MipLevels);
        EXPECT_EQ(uint32_t(vk.samples), d.SampleCount);
        EXPECT_EQ(bool(vk.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT), item.Reinterpret);
        EXPECT_EQ(bool(vk.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT), d.Dim == TextureDimension::Cube || d.Dim == TextureDimension::CubeArray);
        const auto range = vulkan::MakeExternalTextureRange(d);
        EXPECT_EQ(range.baseMipLevel, 0u);
        EXPECT_EQ(range.baseArrayLayer, 0u);
        EXPECT_EQ(range.levelCount, d.MipLevels);
        EXPECT_EQ(range.layerCount, vk.arrayLayers);
        EXPECT_EQ(bool(range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT), !IsDepthStencilFormat(d.Format));
        EXPECT_EQ(bool(range.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT), GetTextureFormatAspects(d.Format).HasFlag(TextureAspect::Stencil));
#endif
    }
}
}  // namespace
}  // namespace radray::render
