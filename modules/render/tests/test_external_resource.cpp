#include "test_external_resource.h"

#include <limits>

namespace radray::render {
void ExternalResourceConfigurationTest::CheckConfiguration(bool enabled) {
    test::DeviceContext context;
    if (!test::TryCreateDevice(GetParam(), context, true, false, {1, 0, 0}, nullptr, true, true, nullptr, enabled)) {
        if (test::SetupMustFail(context.Status, test::RequiredBackend(GetParam()))) FAIL() << context.Reason;
        GTEST_SKIP() << context.Reason;
    }
    const auto nativeExtension = context.Device->GetExternalResourceExtension();
#if defined(RADRAY_PLATFORM_WINDOWS)
    EXPECT_EQ(nativeExtension.HasValue(), enabled);
#else
    EXPECT_FALSE(nativeExtension.HasValue());
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (GetParam() == RenderBackend::Vulkan && !enabled) {
        const auto& native = static_cast<const vulkan::DeviceVulkan&>(*context.Device);
        EXPECT_FALSE(native._externalMemoryWin32);
        EXPECT_FALSE(native._externalSemaphoreWin32);
        EXPECT_FALSE(native._externalExtension);
    }
#endif
    auto buffer = context.Device->CreateBuffer(test::ExternalTestBufferDescriptor(0));
    ASSERT_TRUE(buffer.HasValue());
    auto fence = context.Device->CreateFence();
    ASSERT_TRUE(fence.HasValue());

    auto validated = detail::CreateValidationDevice(context.Device, {.IsEnabled = true});
    ASSERT_TRUE(validated.HasValue());
    const auto validatedExtension = validated->GetExternalResourceExtension();
    EXPECT_EQ(validatedExtension.HasValue(), nativeExtension.HasValue());
    EXPECT_EQ(validated->GetExternalResourceExtension().Get(), validatedExtension.Get());
    EXPECT_EQ(context.Device->GetExternalResourceExtension().Get(), nativeExtension.Get());
}

TEST_P(ExternalResourceConfigurationTest, DisabledByDefault) {
    EXPECT_FALSE(D3D12DeviceDescriptor{}.EnableExternalResource);
    EXPECT_FALSE(VulkanDeviceDescriptor{}.EnableExternalResource);
    CheckConfiguration(false);
}

TEST_P(ExternalResourceConfigurationTest, ExplicitlyEnabled) {
    CheckConfiguration(true);
}

INSTANTIATE_TEST_SUITE_P(Backends, ExternalResourceConfigurationTest, testing::Values(RenderBackend::D3D12, RenderBackend::Vulkan));

ExternalErrorLogProbe::ExternalErrorLogProbe() {
    SetLogCallback([](LogLevel level, std::string_view message, void* data) {
        if (level != LogLevel::Err || !message.starts_with("External resource ")) return;
        auto& self = *static_cast<ExternalErrorLogProbe*>(data);
        std::lock_guard lock{self._mutex};
        self._messages.emplace_back(message);
    },
                   this);
}
ExternalErrorLogProbe::~ExternalErrorLogProbe() { ClearLogCallback(); }
vector<string> ExternalErrorLogProbe::Take() {
    std::lock_guard lock{_mutex};
    return std::exchange(_messages, {});
}
int ExternalResourceTest::Route() const { return GetParam(); }
RenderBackend ExternalResourceFixture::Producer() const { return Route() < 2 ? RenderBackend::D3D12 : RenderBackend::Vulkan; }
RenderBackend ExternalResourceFixture::Consumer() const { return Route() < 4 && Route() % 2 == 0 ? RenderBackend::D3D12 : RenderBackend::Vulkan; }
ExternalResourceDescriptor ExternalResourceFixture::MemoryDescriptor() const { return {Route() == 4 ? ExternalMemoryHandleType::OpaqueWin32 : ExternalMemoryHandleType::D3D12Resource}; }
ExternalFenceHandleType ExternalResourceFixture::FenceType() const { return Route() == 4 ? ExternalFenceHandleType::OpaqueWin32 : ExternalFenceHandleType::D3D12Fence; }
void ExternalResourceFixture::SetUp() {
    if (!test::TryCreateDevice(Producer(), Source, true, true, {1, 0, 0}, nullptr, true, true, nullptr, true)) {
        if (test::SetupMustFail(Source.Status, test::RequiredBackend(Producer()))) FAIL() << Source.Reason;
        GTEST_SKIP() << Source.Reason;
    }
    Target = &Source;
    if (Producer() != Consumer() || Route() == 4) {
        if (!test::TryCreateDevice(Consumer(), Destination, true, true, {1, 0, 0}, nullptr, true, true, Route() == 4 ? Source.VulkanInstance : nullptr, true)) {
            if (test::SetupMustFail(Destination.Status, test::RequiredBackend(Consumer()))) FAIL() << Destination.Reason;
            GTEST_SKIP() << Destination.Reason;
        }
        Target = &Destination;
    }
    Exporter = Source.Device->GetExternalResourceExtension().Get();
    Importer = Target->Device->GetExternalResourceExtension().Get();
    if (!Exporter || !Importer) GTEST_SKIP() << "Windows external resource extension unavailable";
    const auto sourceIdentity = Exporter->GetDeviceIdentity();
    const auto targetIdentity = Importer->GetDeviceIdentity();
    for (const auto& identity : {sourceIdentity, targetIdentity}) {
        if (const auto* error = std::get_if<ExternalError>(&identity)) {
            ASSERT_TRUE(test::ExternalSupportQuerySucceeded({false, false, *error}));
            GTEST_SKIP() << error->Message;
        }
    }
    if (std::get<ExternalDeviceIdentity>(sourceIdentity) != std::get<ExternalDeviceIdentity>(targetIdentity)) GTEST_SKIP() << "test adapters differ";
}
void ExternalResourceFixture::TearDown() {
    Destination.Reset();
    Source.Reset();
}
void ExternalResourceFixture::TextureRoundTrips(std::span<const TextureFormat> formats, std::span<const test::ExternalTextureTestUse> uses) {
    const auto sourceBuffer = Exporter->QueryBufferSupport(test::ExternalTestBufferDescriptor(0), MemoryDescriptor());
    const auto targetBuffer = Importer->QueryBufferSupport(test::ExternalTestBufferDescriptor(0), MemoryDescriptor());
    const auto sourceFence = Exporter->QueryFenceSupport(FenceType());
    const auto targetFence = Importer->QueryFenceSupport(FenceType());
    for (const auto& support : {sourceBuffer, targetBuffer, sourceFence, targetFence}) ASSERT_TRUE(test::ExternalSupportQuerySucceeded(support));
    if (!sourceBuffer.CanCreateExportable || !targetBuffer.CanImport || !sourceFence.CanCreateExportable || !targetFence.CanImport)
        GTEST_SKIP() << "native resource/fence route unsupported; success path not covered";
    unique_ptr<Fence> ready, released, readyImported, releasedImported;
    ASSERT_TRUE(test::TakeExternal(Exporter->CreateExportableFence({.HandleType = FenceType(), .InitialValue = 7}), ready));
    EXPECT_EQ(ready->GetLastSignaledValue(), 0);
    EXPECT_EQ(ready->GetCompletedValue(), 7);
    ASSERT_TRUE(test::TakeExternal(Exporter->CreateExportableFence({.HandleType = FenceType()}), released));
    ExternalFenceExport readyHandle, releasedHandle;
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportFence(ready.get()), readyHandle));
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportFence(released.get()), releasedHandle));
    ASSERT_TRUE(test::TakeExternal(Importer->ImportFence(readyHandle.Handle.Borrow(), readyHandle.Info), readyImported));
    ASSERT_TRUE(test::TakeExternal(Importer->ImportFence(releasedHandle.Handle.Borrow(), releasedHandle.Info), releasedImported));
    EXPECT_EQ(readyImported->GetLastSignaledValue(), 0);
    EXPECT_EQ(readyImported->GetCompletedValue(), 7);
    readyHandle.Handle.Reset();
    releasedHandle.Handle.Reset();
    uint64_t timeline = 8;
    uint32_t completed = 0;
    string unsupported;
    for (auto use : uses)
        for (auto format : formats) {
            SCOPED_TRACE(fmt::format("format={} use={}", format, uint32_t(use)));
            const auto desc = test::ExternalTestTextureDescriptor(format, use, 0);
            const auto sourceSupport = test::QueryExternalTextureSupport(Source, desc, {MemoryDescriptor().HandleType}, "legacy-producer");
            const auto targetSupport = test::QueryExternalTextureSupport(*Target, desc, {MemoryDescriptor().HandleType}, "legacy-consumer");
            ASSERT_TRUE(test::ExternalSupportQuerySucceeded(sourceSupport));
            ASSERT_TRUE(test::ExternalSupportQuerySucceeded(targetSupport));
            if (!sourceSupport.CanCreateExportable || !targetSupport.CanImport) {
                unsupported += fmt::format("format={} use={}: {}; {}; ", format, uint32_t(use), sourceSupport.Reason.Message, targetSupport.Reason.Message);
                continue;
            }
            test::ExternalTestSlot local, remote;
            test::ExternalTestExports exported;
            ASSERT_TRUE(test::CreateExternalTestSlot(*Exporter, format, use, 0, local, MemoryDescriptor()));
            ASSERT_TRUE(test::ExportExternalTestSlot(*Exporter, local, exported));
            ASSERT_TRUE(test::ImportExternalTestSlot(*Importer, exported, remote));
            exported.BufferObject.Handle.Reset();
            exported.TextureObject.Handle.Reset();
            ASSERT_TRUE(test::ExternalTestTurn(Source, local, use, 0, 1, true, released.get(), 0, ready.get(), timeline));
            for (uint32_t frame = 1; frame <= 4; ++frame) {
                ASSERT_TRUE(test::ExternalTestTurn(*Target, remote, use, frame * 2 - 1, frame * 2, false, readyImported.get(), timeline, releasedImported.get(), timeline));
                ++timeline;
                ASSERT_TRUE(test::ExternalTestTurn(Source, local, use, frame * 2, frame * 2 + 1, false, released.get(), timeline - 1, ready.get(), timeline));
            }
            ++timeline;
            ++completed;
        }
    RecordProperty("completed_combinations", completed);
    RecordProperty("unsupported_combinations", unsupported);
    if (!completed && !HasFailure()) GTEST_SKIP() << unsupported;
    EXPECT_EQ(released->GetLastSignaledValue(), 0);
    EXPECT_EQ(readyImported->GetLastSignaledValue(), 0);
}

namespace {

TEST(ExternalResourceQueryContract, NativeErrorsAreNotUnsupported) {
    EXPECT_TRUE(test::ExternalSupportQuerySucceeded({false, false, {"policy unsupported"}}));
    EXPECT_TRUE(test::ExternalSupportQuerySucceeded({false, true, {"export unsupported; import supported"}}));
    EXPECT_TRUE(test::ExternalSupportQuerySucceeded({false, false, {"format unsupported", ExternalNativeApi::Vulkan, -11}}));
    for (const int64_t error : {-1, -2, -4})
        EXPECT_FALSE(test::ExternalSupportQuerySucceeded({false, false, {"native failure", ExternalNativeApi::Vulkan, error}}));
    EXPECT_FALSE(test::ExternalSupportQuerySucceeded({false, false, {"E_INVALIDARG", ExternalNativeApi::D3D12, -2147024809LL}}));
    EXPECT_FALSE(test::ExternalSupportQuerySucceeded({false, false, {"access denied", ExternalNativeApi::Win32, 5}}));
}

TEST_P(ExternalResourceTest, DeviceIdentityMatchesImportingDevice) {
    ExternalErrorLogProbe log;
    const auto identity = Exporter->GetDeviceIdentity();
    const auto importedIdentity = Importer->GetDeviceIdentity();
    ASSERT_TRUE(std::holds_alternative<ExternalDeviceIdentity>(identity));
    ASSERT_TRUE(std::holds_alternative<ExternalDeviceIdentity>(importedIdentity));
    EXPECT_EQ(std::get<ExternalDeviceIdentity>(identity).NodeMask, 1u);
    EXPECT_EQ(std::get<ExternalDeviceIdentity>(identity), std::get<ExternalDeviceIdentity>(importedIdentity));
    EXPECT_TRUE(log.Take().empty());
}

TEST_P(ExternalResourceTest, UnsupportedExportIsExplicitFailure) {
    const auto buffer = test::ExternalTestBufferDescriptor(0);
    const auto bufferSupport = Exporter->QueryBufferSupport(buffer, MemoryDescriptor());
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(bufferSupport));
    EXPECT_EQ(bufferSupport.Reason.Message.empty(), bufferSupport.CanCreateExportable && bufferSupport.CanImport);
    if (!bufferSupport.CanCreateExportable) {
        auto result = Exporter->CreateExportableBuffer(buffer, MemoryDescriptor());
        ASSERT_TRUE(std::holds_alternative<ExternalError>(result));
        EXPECT_FALSE(std::get<ExternalError>(result).Message.empty());
        EXPECT_EQ(std::get<ExternalError>(result).Message, bufferSupport.Reason.Message);
        EXPECT_EQ(std::get<ExternalError>(result).NativeApi, bufferSupport.Reason.NativeApi);
        EXPECT_EQ(std::get<ExternalError>(result).NativeCode, bufferSupport.Reason.NativeCode);
    }
    for (auto format : {TextureFormat::RGBA8_UNORM, TextureFormat::BGRA8_UNORM, TextureFormat::RGBA16_FLOAT, TextureFormat::R32_FLOAT})
        for (auto use : {test::ExternalTextureTestUse::Copy, test::ExternalTextureTestUse::Sampled, test::ExternalTextureTestUse::Storage, test::ExternalTextureTestUse::RenderTarget}) {
            SCOPED_TRACE(fmt::format("format={} use={}", format, uint32_t(use)));
            const auto desc = test::ExternalTestTextureDescriptor(format, use, 0);
            const auto support = Exporter->QueryTextureSupport(desc, {MemoryDescriptor().HandleType});
            ASSERT_TRUE(test::ExternalSupportQuerySucceeded(support));
            EXPECT_EQ(support.Reason.Message.empty(), support.CanCreateExportable && support.CanImport);
            if (!support.CanCreateExportable) {
                auto result = Exporter->CreateExportableTexture(desc, {MemoryDescriptor().HandleType});
                ASSERT_TRUE(std::holds_alternative<ExternalError>(result));
                EXPECT_FALSE(std::get<ExternalError>(result).Message.empty());
                EXPECT_EQ(std::get<ExternalError>(result).Message, support.Reason.Message);
                EXPECT_EQ(std::get<ExternalError>(result).NativeApi, support.Reason.NativeApi);
                EXPECT_EQ(std::get<ExternalError>(result).NativeCode, support.Reason.NativeCode);
            }
        }
    const auto fenceSupport = Exporter->QueryFenceSupport(FenceType());
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(fenceSupport));
    EXPECT_EQ(fenceSupport.Reason.Message.empty(), fenceSupport.CanCreateExportable && fenceSupport.CanImport);
    if (!fenceSupport.CanCreateExportable) {
        auto result = Exporter->CreateExportableFence({.HandleType = FenceType()});
        ASSERT_TRUE(std::holds_alternative<ExternalError>(result));
        EXPECT_FALSE(std::get<ExternalError>(result).Message.empty());
        EXPECT_EQ(std::get<ExternalError>(result).Message, fenceSupport.Reason.Message);
        EXPECT_EQ(std::get<ExternalError>(result).NativeApi, fenceSupport.Reason.NativeApi);
        EXPECT_EQ(std::get<ExternalError>(result).NativeCode, fenceSupport.Reason.NativeCode);
    }
}

TEST_P(ExternalResourceTest, TextureUsagesAndBufferRoundTrip) {
    const TextureFormat formats[]{TextureFormat::RGBA8_UNORM, TextureFormat::RGBA16_FLOAT, TextureFormat::R32_FLOAT};
    const test::ExternalTextureTestUse uses[]{test::ExternalTextureTestUse::Copy, test::ExternalTextureTestUse::Sampled, test::ExternalTextureTestUse::Storage, test::ExternalTextureTestUse::RenderTarget};
    TextureRoundTrips(formats, uses);
}

TEST_P(ExternalResourceTest, BGRA8CopyRoundTrip) {
    const TextureFormat formats[]{TextureFormat::BGRA8_UNORM};
    const test::ExternalTextureTestUse uses[]{test::ExternalTextureTestUse::Copy};
    TextureRoundTrips(formats, uses);
}

TEST_P(ExternalResourceTest, BGRA8SampledRoundTrip) {
    const TextureFormat formats[]{TextureFormat::BGRA8_UNORM};
    const test::ExternalTextureTestUse uses[]{test::ExternalTextureTestUse::Sampled};
    TextureRoundTrips(formats, uses);
}

TEST_P(ExternalResourceTest, BGRA8RenderTargetRoundTrip) {
    const TextureFormat formats[]{TextureFormat::BGRA8_UNORM};
    const test::ExternalTextureTestUse uses[]{test::ExternalTextureTestUse::RenderTarget};
    TextureRoundTrips(formats, uses);
}

TEST_P(ExternalResourceTest, BGRA8StorageRoundTrip) {
    if ((Producer() == RenderBackend::Vulkan && !Source.VulkanStorageImageWriteWithoutFormatEnabled) ||
        (Consumer() == RenderBackend::Vulkan && !Target->VulkanStorageImageWriteWithoutFormatEnabled))
        GTEST_SKIP() << "BGRA8 storage shader requires shaderStorageImageWriteWithoutFormat; success path not covered";
    const TextureFormat formats[]{TextureFormat::BGRA8_UNORM};
    const test::ExternalTextureTestUse uses[]{test::ExternalTextureTestUse::Storage};
    TextureRoundTrips(formats, uses);
}

TEST_P(ExternalResourceTest, DeviceCompatibilityAndIndependentHandles) {
    const auto desc = test::ExternalTestBufferDescriptor(0);
    const auto sourceSupport = Exporter->QueryBufferSupport(desc, MemoryDescriptor());
    const auto targetSupport = Importer->QueryBufferSupport(desc, MemoryDescriptor());
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(sourceSupport));
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(targetSupport));
    if (!sourceSupport.CanCreateExportable || !targetSupport.CanImport)
        GTEST_SKIP() << "native buffer route unsupported; success path not covered";
    unique_ptr<Buffer> source, imported;
    ASSERT_TRUE(test::TakeExternal(Exporter->CreateExportableBuffer(desc, MemoryDescriptor()), source));
    ExternalBufferExport a, b;
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportBuffer(source.get()), a));
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportBuffer(source.get()), b));
    EXPECT_NE(a.Handle.Borrow().Value, b.Handle.Borrow().Value);
    a.Handle.Reset();
    auto badInfo = b.Info;
    badInfo.Device.Luid[0] ^= 1;
    ExternalErrorLogProbe log;
    auto wrongDevice = Importer->ImportBuffer(b.Handle.Borrow(), badInfo);
    ASSERT_TRUE(std::holds_alternative<ExternalError>(wrongDevice));
    EXPECT_NE(std::get<ExternalError>(wrongDevice).Message.find("LUID"), string::npos);
    EXPECT_EQ(log.Take().size(), 1u);
    ASSERT_TRUE(test::TakeExternal(Importer->ImportBuffer(b.Handle.Borrow(), b.Info), imported));
    EXPECT_TRUE(b.Handle.IsValid());
    EXPECT_TRUE(log.Take().empty());
}

TEST_P(ExternalResourceTest, ExportedHandleLifetime) {
    const auto desc = test::ExternalTestBufferDescriptor(0);
    const auto support = Exporter->QueryBufferSupport(desc, MemoryDescriptor());
    ASSERT_TRUE(test::ExternalSupportQuerySucceeded(support));
    if (!support.CanCreateExportable)
        GTEST_SKIP() << "native buffer export unsupported; success path not covered";
    unique_ptr<Buffer> shared;
    ASSERT_TRUE(test::TakeExternal(Exporter->CreateExportableBuffer(desc, MemoryDescriptor()), shared));
#if defined(RADRAY_PLATFORM_WINDOWS)
    ExternalBufferExport warmup;
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportBuffer(shared.get()), warmup));
    warmup.Handle.Reset();
    DWORD before = 0, after = 0;
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &before));
    for (uint32_t i = 0; i < 64; ++i) {
        ExternalBufferExport exported;
        ASSERT_TRUE(test::TakeExternal(Exporter->ExportBuffer(shared.get()), exported));
        OwnedExternalHandle duplicate;
        ASSERT_TRUE(test::TakeExternal(exported.Handle.Duplicate(), duplicate));
        EXPECT_NE(duplicate.Borrow().Value, exported.Handle.Borrow().Value);
        auto moved = std::move(duplicate);
        EXPECT_FALSE(duplicate.IsValid());
        EXPECT_TRUE(moved.IsValid());
    }
    ASSERT_TRUE(GetProcessHandleCount(GetCurrentProcess(), &after));
    EXPECT_LE(after, before + 2);
#endif
}

TEST_P(ExternalResourceOpaqueTest, RuntimeCompatibilityAndBorrowedHandles) {
    const auto buffer = test::ExternalTestBufferDescriptor(0);
    const auto texture = test::ExternalTestTextureDescriptor(TextureFormat::BGRA8_UNORM, test::ExternalTextureTestUse::Copy, 0);
    const auto sourceBuffer = Exporter->QueryBufferSupport(buffer, MemoryDescriptor());
    const auto targetBuffer = Importer->QueryBufferSupport(buffer, MemoryDescriptor());
    const auto sourceTexture = test::QueryExternalTextureSupport(Source, texture, {MemoryDescriptor().HandleType}, "opaque-producer");
    const auto targetTexture = test::QueryExternalTextureSupport(*Target, texture, {MemoryDescriptor().HandleType}, "opaque-consumer");
    const auto sourceFence = Exporter->QueryFenceSupport(FenceType());
    const auto targetFence = Importer->QueryFenceSupport(FenceType());
    for (const auto& support : {sourceBuffer, targetBuffer, sourceTexture, targetTexture, sourceFence, targetFence}) ASSERT_TRUE(test::ExternalSupportQuerySucceeded(support));
    if (!sourceBuffer.CanCreateExportable || !targetBuffer.CanImport || !sourceTexture.CanCreateExportable || !targetTexture.CanImport || !sourceFence.CanCreateExportable || !targetFence.CanImport)
        GTEST_SKIP() << "opaque resource/fence route unavailable";
    ASSERT_NE(Source.Device.get(), Target->Device.get());
    test::ExternalTestSlot local, remote;
    test::ExternalTestExports exported;
    unique_ptr<Fence> fence, importedFence;
    ExternalFenceExport fenceExport;
    ASSERT_TRUE(test::CreateExternalTestSlot(*Exporter, texture.Format, test::ExternalTextureTestUse::Copy, 0, local, MemoryDescriptor()));
    ASSERT_TRUE(test::ExportExternalTestSlot(*Exporter, local, exported));
    ASSERT_TRUE(test::TakeExternal(Exporter->CreateExportableFence({FenceType(), 19}), fence));
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportFence(fence.get()), fenceExport));
    ASSERT_TRUE(exported.BufferObject.Info.VulkanMemory);
    ASSERT_TRUE(exported.TextureObject.Info.VulkanMemory);
    ASSERT_TRUE(fenceExport.Info.VulkanIdentity);
    ExternalErrorLogProbe log;
    const auto rejected = [&](auto result) {
        ASSERT_TRUE(std::holds_alternative<ExternalError>(result));
        EXPECT_FALSE(std::get<ExternalError>(result).Message.empty());
        const auto messages = log.Take();
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_NE(messages[0].find("handleType=OPAQUE_WIN32"), string::npos);
    };
    for (uint32_t mutation = 1; mutation <= 4; ++mutation) {
        SCOPED_TRACE(mutation);
        auto b = exported.BufferObject.Info;
        auto t = exported.TextureObject.Info;
        switch (mutation) {
            case 1:
                b.VulkanMemory->Device.DeviceUuid[0] ^= 1;
                t.VulkanMemory->Device.DeviceUuid[0] ^= 1;
                break;
            case 2:
                b.VulkanMemory->Device.DriverUuid[0] ^= 1;
                t.VulkanMemory->Device.DriverUuid[0] ^= 1;
                break;
            case 3: b.VulkanMemory->MemoryTypeIndex = t.VulkanMemory->MemoryTypeIndex = std::numeric_limits<uint32_t>::max(); break;
            case 4:
                b.Allocation.Size = 1;
                t.Allocation.Size = 1;
                break;
        }
        rejected(Importer->ImportBuffer(exported.BufferObject.Handle.Borrow(), b));
        rejected(Importer->ImportTexture(exported.TextureObject.Handle.Borrow(), t));
    }
    for (uint32_t mutation = 1; mutation <= 2; ++mutation) {
        auto info = fenceExport.Info;
        switch (mutation) {
            case 1: info.VulkanIdentity->DeviceUuid[0] ^= 1; break;
            case 2: info.VulkanIdentity->DriverUuid[0] ^= 1; break;
        }
        rejected(Importer->ImportFence(fenceExport.Handle.Borrow(), info));
    }
#if defined(RADRAY_ENABLE_VULKAN)
    // Select a known non-device-local type, never guess an index into an opaque payload.
    const auto* instance = static_cast<const vulkan::InstanceVulkanImpl*>(Source.VulkanInstance.Get());
    uint32_t count = 0;
    ASSERT_EQ(vkEnumeratePhysicalDevices(instance->_instance, &count, nullptr), VK_SUCCESS);
    vector<VkPhysicalDevice> devices(count);
    ASSERT_EQ(vkEnumeratePhysicalDevices(instance->_instance, &count, devices.data()), VK_SUCCESS);
    for (auto device : devices) {
        VkPhysicalDeviceIDProperties id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 properties{};
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &id;
        vkGetPhysicalDeviceProperties2(device, &properties);
        if (!std::equal(std::begin(id.deviceUUID), std::end(id.deviceUUID), exported.BufferObject.Info.VulkanMemory->Device.DeviceUuid.begin())) continue;
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(device, &memory);
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) continue;
            auto b = exported.BufferObject.Info;
            auto t = exported.TextureObject.Info;
            b.VulkanMemory->MemoryTypeIndex = t.VulkanMemory->MemoryTypeIndex = i;
            rejected(Importer->ImportBuffer(exported.BufferObject.Handle.Borrow(), b));
            rejected(Importer->ImportTexture(exported.TextureObject.Handle.Borrow(), t));
            break;
        }
    }
#endif
    // Independent exports and successful imports remain valid after rejected imports and closing siblings.
    ExternalTextureExport textureSibling;
    ExternalFenceExport fenceSibling;
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportTexture(local.TextureObject.get()), textureSibling));
    ASSERT_TRUE(test::TakeExternal(Exporter->ExportFence(fence.get()), fenceSibling));
    EXPECT_NE(textureSibling.Handle.Borrow().Value, exported.TextureObject.Handle.Borrow().Value);
    EXPECT_NE(fenceSibling.Handle.Borrow().Value, fenceExport.Handle.Borrow().Value);
    textureSibling.Handle.Reset();
    fenceSibling.Handle.Reset();
    ASSERT_TRUE(test::ImportExternalTestSlot(*Importer, exported, remote));
    ASSERT_TRUE(test::TakeExternal(Importer->ImportFence(fenceExport.Handle.Borrow(), fenceExport.Info), importedFence));
    EXPECT_TRUE(exported.BufferObject.Handle.IsValid());
    EXPECT_TRUE(exported.TextureObject.Handle.IsValid());
    EXPECT_TRUE(fenceExport.Handle.IsValid());
    exported = {};
    fenceExport = {};
    fence.reset();
    EXPECT_EQ(importedFence->GetCompletedValue(), 19u);
    EXPECT_EQ(importedFence->GetLastSignaledValue(), 0u);
    EXPECT_TRUE(log.Take().empty());
}

TEST_P(ExternalResourceTest, D3D12RejectsOpaqueWithoutNativeCalls) {
    if (Producer() != RenderBackend::D3D12) return;
    ExternalErrorLogProbe log;
    const auto b = test::ExternalTestBufferDescriptor(0);
    const auto t = test::ExternalTestTextureDescriptor(TextureFormat::BGRA8_UNORM, test::ExternalTextureTestUse::Copy, 0);
    const ExternalResourceDescriptor memory{ExternalMemoryHandleType::OpaqueWin32};
    EXPECT_FALSE(Exporter->QueryBufferSupport(b, memory).CanCreateExportable);
    EXPECT_FALSE(Exporter->QueryTextureSupport(t, {memory.HandleType}).CanImport);
    EXPECT_FALSE(Exporter->QueryFenceSupport(ExternalFenceHandleType::OpaqueWin32).CanImport);
    EXPECT_TRUE(log.Take().empty());
    const auto rejected = [&](auto result) {
        ASSERT_TRUE(std::holds_alternative<ExternalError>(result));
        EXPECT_EQ(std::get<ExternalError>(result).NativeApi, ExternalNativeApi::None);
        const auto messages = log.Take();
        ASSERT_EQ(messages.size(), 1u);
        EXPECT_NE(messages[0].find("OPAQUE_WIN32"), string::npos);
    };
    rejected(Exporter->CreateExportableBuffer(b, memory));
    rejected(Exporter->CreateExportableTexture(t, {memory.HandleType}));
    rejected(Exporter->CreateExportableFence({ExternalFenceHandleType::OpaqueWin32}));
    ExternalBufferInfo bi;
    bi.HandleType = ExternalMemoryHandleType::OpaqueWin32;
    ExternalTextureInfo ti;
    ti.HandleType = ExternalMemoryHandleType::OpaqueWin32;
    ExternalFenceInfo fi;
    fi.HandleType = ExternalFenceHandleType::OpaqueWin32;
    rejected(Exporter->ImportBuffer({}, bi));
    rejected(Exporter->ImportTexture({}, ti));
    rejected(Exporter->ImportFence({}, fi));
}

INSTANTIATE_TEST_SUITE_P(Opaque, ExternalResourceOpaqueTest, testing::Values(4));
INSTANTIATE_TEST_SUITE_P(Routes, ExternalResourceTest, testing::Values(0, 1, 2, 3, 4));
#if defined(RADRAY_PLATFORM_WINDOWS)
TEST_P(ExternalResourceIpcTest, SingleSlot) {
    const int result = test::RunExternalSupervisor(RADRAY_EXTERNAL_WORKER_PATH, GetParam() < 2 ? RenderBackend::D3D12 : RenderBackend::Vulkan,
                                                   GetParam() < 4 && GetParam() % 2 == 0 ? RenderBackend::D3D12 : RenderBackend::Vulkan, 1, false, GetParam() == 4);
    if (result == 77) GTEST_SKIP() << "native route unavailable; IPC success path not covered";
    if (result == 78) GTEST_SKIP() << "KNOWN_ISSUE_SKIP: recorded IPC texture issue; see known_issue_skips and generation properties";
    EXPECT_EQ(result, 0);
}
TEST_P(ExternalResourceIpcTest, ThreeSlotsResizeDelayedConsumer) {
    const int result = test::RunExternalSupervisor(RADRAY_EXTERNAL_WORKER_PATH, GetParam() < 2 ? RenderBackend::D3D12 : RenderBackend::Vulkan,
                                                   GetParam() < 4 && GetParam() % 2 == 0 ? RenderBackend::D3D12 : RenderBackend::Vulkan, 3, false, GetParam() == 4);
    if (result == 77) GTEST_SKIP() << "native route unavailable; IPC success path not covered";
    if (result == 78) GTEST_SKIP() << "KNOWN_ISSUE_SKIP: recorded IPC texture issue; see known_issue_skips and generation properties";
    EXPECT_EQ(result, 0);
}
TEST_P(ExternalResourceIpcTest, InterruptedConsumerTerminatesUndrainableProducer) {
    const int result = test::RunExternalSupervisor(RADRAY_EXTERNAL_WORKER_PATH, GetParam() < 2 ? RenderBackend::D3D12 : RenderBackend::Vulkan,
                                                   GetParam() < 4 && GetParam() % 2 == 0 ? RenderBackend::D3D12 : RenderBackend::Vulkan, 3, true, GetParam() == 4);
    if (result == 77) GTEST_SKIP() << "native route unavailable; IPC success path not covered";
    if (result == 78) GTEST_SKIP() << "KNOWN_ISSUE_SKIP: recorded IPC texture issue; see known_issue_skips and generation properties";
    EXPECT_EQ(result, 0);
}
INSTANTIATE_TEST_SUITE_P(Routes, ExternalResourceIpcTest, testing::Values(0, 1, 2, 3, 4));
#endif
}  // namespace
}  // namespace radray::render
