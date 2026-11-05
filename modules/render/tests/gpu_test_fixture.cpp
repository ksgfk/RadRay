#include "gpu_test_fixture.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <optional>

#include <radray/file.h>

namespace radray::render::test {
namespace {

std::string_view EnvironmentValue(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value ? std::string_view{value} : std::string_view{};
}

bool IsRequired(RenderBackend backend) noexcept {
    const std::string_view expected = backend == RenderBackend::D3D12 ? "d3d12" : "vulkan";
    std::string_view list = EnvironmentValue("RADRAY_TEST_REQUIRED_BACKENDS");
    while (!list.empty()) {
        const size_t comma = list.find(',');
        std::string_view token = list.substr(0, comma);
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        const bool matches = std::ranges::equal(token, expected, [](char a, char b) {
            return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
        });
        if (matches) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

bool IsGpuValidationRequested() noexcept {
    return EnvironmentValue("RADRAY_TEST_GPU_VALIDATION") == "1";
}

// False when the variable is set but is not an unsigned integer.
bool ReadAdapterIndex(const char* variable, std::optional<uint32_t>& index) noexcept {
    const std::string_view text = EnvironmentValue(variable);
    if (text.empty()) return true;
    uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) return false;
    index = value;
    return true;
}

void SkipTest(std::string_view reason) {
    GTEST_SKIP() << reason;
}

void FailTest(std::string_view reason) {
    GTEST_FAIL() << reason;
}

}  // namespace

string BackendParamName(const testing::TestParamInfo<RenderBackend>& info) {
    return info.param == RenderBackend::D3D12 ? "D3D12" : "Vulkan";
}

void GpuTest::TearDown() {
    for (const auto& device : _devices) DirectQueue(*device).Wait();
    _devices.clear();
    _dxgiFactory.reset();
    _dxgiContext.reset();
    if (_vulkanInstance) {
        InstanceVulkan::ShutdownEnv();
        _vulkanInstance = nullptr;
    }
    EXPECT_EQ(_nativeErrors.load(), 0u) << "native validation reported errors; see the log above";
    EXPECT_EQ(_rhiErrors.load(), 0u) << "RHI validation reported diagnostics the test did not expect";
}

Nullable<Device*> GpuTest::OpenDevice(RenderBackend backend, bool externalResource) {
    string reason;
    OpenStatus status = OpenStatus::Unavailable;
    if (backend == RenderBackend::D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
        status = CreateD3D12Device(externalResource, reason);
#else
        reason = "D3D12 backend is not built";
#endif
    } else {
#if defined(RADRAY_ENABLE_VULKAN)
        status = CreateVulkanDevice(externalResource, reason);
#else
        reason = "Vulkan backend is not built";
#endif
    }
    if (status == OpenStatus::Opened) return _devices.back().get();
    if (status == OpenStatus::Unavailable && !IsRequired(backend))
        SkipTest(reason);
    else
        FailTest(reason);
    return nullptr;
}

GpuTest::OpenStatus GpuTest::CreateD3D12Device([[maybe_unused]] bool externalResource, string& reason) {
#if defined(RADRAY_ENABLE_D3D12)
    if (!_dxgiFactory) {
        auto context = DXGIDiagnosticContext::Create();
        if (!context) {
            reason = "DXGI diagnostic context creation failed";
            return OpenStatus::Failed;
        }
        _dxgiContext = context.Release();
        auto factory = DXGIFactory::Create({.IsEnableDebugLayer = true,
                                            .IsEnableGpuBasedValid = IsGpuValidationRequested(),
                                            .LogCallback = OnNativeMessage,
                                            .LogUserData = this,
                                            .DiagnosticContext = _dxgiContext});
        if (!factory || !factory->IsValidationEnabled()) {
            reason = "DXGI factory with the D3D12 debug layer is unavailable";
            return OpenStatus::Unavailable;
        }
        _dxgiFactory = factory.Release();
    }
    std::optional<uint32_t> adapter;
    if (!ReadAdapterIndex("RADRAY_TEST_D3D12_ADAPTER_INDEX", adapter)) {
        reason = "RADRAY_TEST_D3D12_ADAPTER_INDEX must be an unsigned integer";
        return OpenStatus::Failed;
    }
    const auto adapters = _dxgiFactory->GetAdapters();
    if (adapter && std::ranges::none_of(adapters, [&](const auto& info) { return info.Index == *adapter; })) {
        reason = fmt::format("D3D12 adapter {} does not exist", *adapter);
        return OpenStatus::Failed;
    }
    if (!adapter) adapter = _dxgiFactory->SelectHighPerformanceAdapter();
    if (!adapter) {
        reason = "no D3D12 adapter is available";
        return OpenStatus::Unavailable;
    }
    const D3D12DeviceDescriptor desc{.Factory = _dxgiFactory.get(), .AdapterIndex = adapter, .EnableExternalResource = externalResource};
    return AddDevice(Device::Create(desc, {.IsEnabled = true, .LogCallback = OnRhiMessage, .LogUserData = this}), reason);
#else
    reason = "D3D12 backend is not built";
    return OpenStatus::Unavailable;
#endif
}

GpuTest::OpenStatus GpuTest::CreateVulkanDevice([[maybe_unused]] bool externalResource, string& reason) {
#if defined(RADRAY_ENABLE_VULKAN)
    if (!_vulkanInstance) {
        _vulkanInstance = InstanceVulkan::InitEnv({.AppName = "radray_render_test",
                                                   .EngineName = "radray",
                                                   .IsEnableDebugLayer = true,
                                                   .IsEnableGpuBasedValid = IsGpuValidationRequested(),
                                                   .IsEnableSynchronizationValidation = true,
                                                   .LogCallback = OnNativeMessage,
                                                   .LogUserData = this});
        if (!_vulkanInstance) {
            reason = "Vulkan 1.2 instance with validation is unavailable";
            return OpenStatus::Unavailable;
        }
        if (!_vulkanInstance->IsValidationEnabled()) {
            reason = "Vulkan validation layer is unavailable";
            return OpenStatus::Unavailable;
        }
    }
    std::optional<uint32_t> adapter;
    if (!ReadAdapterIndex("RADRAY_TEST_VULKAN_ADAPTER_INDEX", adapter)) {
        reason = "RADRAY_TEST_VULKAN_ADAPTER_INDEX must be an unsigned integer";
        return OpenStatus::Failed;
    }
    const auto adapters = _vulkanInstance->GetPhysicalDevices();
    if (adapter && std::ranges::none_of(adapters, [&](const auto& info) { return info.Index == *adapter; })) {
        reason = fmt::format("Vulkan physical device {} does not exist", *adapter);
        return OpenStatus::Failed;
    }
    if (!adapter) adapter = _vulkanInstance->SelectHighPerformancePhysicalDevice();
    if (!adapter) {
        reason = "no Vulkan physical device is available";
        return OpenStatus::Unavailable;
    }
    const VulkanCommandQueueDescriptor queues[]{{QueueType::Direct, 1}};
    const VulkanDeviceDescriptor desc{.PhysicalDeviceIndex = adapter, .Queues = queues, .EnableExternalResource = externalResource};
    return AddDevice(Device::Create(desc, {.IsEnabled = true, .LogCallback = OnRhiMessage, .LogUserData = this}), reason);
#else
    reason = "Vulkan backend is not built";
    return OpenStatus::Unavailable;
#endif
}

GpuTest::OpenStatus GpuTest::AddDevice(Nullable<shared_ptr<Device>> device, string& reason) {
    if (!device) {
        reason = "device creation failed on an available adapter";
        return OpenStatus::Failed;
    }
    if (!device->GetCommandQueue(QueueType::Direct, 0)) {
        reason = "device has no direct queue";
        return OpenStatus::Failed;
    }
    _devices.push_back(device.Release());
    return OpenStatus::Opened;
}

void GpuTest::OnNativeMessage(LogLevel level, std::string_view message, void* userData) {
    if (level != LogLevel::Err && level != LogLevel::Critical) return;
    static_cast<GpuTest*>(userData)->_nativeErrors.fetch_add(1, std::memory_order_relaxed);
    RADRAY_ERR_LOG("native validation: {}", message);
}

void GpuTest::OnRhiMessage(LogLevel level, std::string_view message, void* userData) {
    if (level != LogLevel::Err && level != LogLevel::Critical) return;
    static_cast<GpuTest*>(userData)->_rhiErrors.fetch_add(1, std::memory_order_relaxed);
    RADRAY_WARN_LOG("RHI validation: {}", message);
}

void BackendTest::SetUp() {
    _device = OpenDevice(GetParam()).Get();
}

CommandQueue& DirectQueue(Device& device) {
    return *device.GetCommandQueue(QueueType::Direct, 0).Get();
}

Nullable<unique_ptr<Shader>> LoadShader(Device& device, std::string_view source, std::string_view entry, ShaderStages stages) {
    const bool dxil = device.GetBackend() == RenderBackend::D3D12;
    const auto path = std::filesystem::path{RADRAY_TEST_SHADER_DIR} / "shader_bytecode" /
                      fmt::format("{}.{}.{}.bin", source, entry, dxil ? "dxil" : "spirv");
    const auto bytes = ReadBinaryFile(path);
    if (!bytes || bytes->empty()) {
        ADD_FAILURE() << "missing shader fixture " << path.string() << "; run tools/compile_test_shaders.py";
        return nullptr;
    }
    return device.CreateShader({*bytes, dxil ? ShaderBlobCategory::DXIL : ShaderBlobCategory::SPIRV, stages});
}

Nullable<unique_ptr<Buffer>> CreateUploadBuffer(Device& device, std::span<const byte> data, BufferUses usage) {
    auto buffer = device.CreateBuffer({.Size = data.size(), .Memory = MemoryType::Upload, .Usage = usage | BufferUse::MapWrite});
    if (!buffer) return nullptr;
    void* mapped = buffer->Map(0, data.size());
    if (!mapped) return nullptr;
    std::memcpy(mapped, data.data(), data.size());
    buffer->FlushMappedRange({0, data.size()});
    buffer->Unmap();
    return buffer;
}

Nullable<unique_ptr<Buffer>> CreateReadbackBuffer(Device& device, uint64_t size) {
    return device.CreateBuffer({.Size = size, .Memory = MemoryType::ReadBack, .Usage = BufferUse::CopyDestination | BufferUse::MapRead});
}

vector<byte> MapAndRead(Buffer& readback) {
    const uint64_t size = readback.GetDesc().Size;
    const auto* mapped = static_cast<const byte*>(readback.Map(0, size));
    if (!mapped) {
        ADD_FAILURE() << "readback buffer could not be mapped";
        return {};
    }
    readback.InvalidateMappedRange({0, size});
    vector<byte> result(mapped, mapped + size);
    readback.Unmap();
    return result;
}

uint32_t CopyRowPitch(const Device& device, uint32_t width, uint32_t bytesPerTexel) noexcept {
    const uint32_t alignment = std::max(1u, device.GetCapabilities().Detail.TextureDataPitchAlignment);
    return (width * bytesPerTexel + alignment - 1) / alignment * alignment;
}

Recording BeginRecording(Device& device) {
    Recording recording;
    recording.Allocator = device.CreateCommandAllocator(&DirectQueue(device)).Unwrap();
    recording.Commands = device.CreateCommandBuffer(recording.Allocator.get()).Unwrap();
    recording.Commands->Begin();
    return recording;
}

void SubmitAndWait(Device& device, Recording& recording) {
    recording.Commands->End();
    CommandBuffer* commands[]{recording.Commands.get()};
    auto& queue = DirectQueue(device);
    queue.Submit({.CmdBuffers = commands});
    queue.Wait();
}

void Barrier(CommandBuffer& commands, std::initializer_list<ResourceBarrierDescriptor> barriers) {
    commands.ResourceBarrier(std::span{barriers.begin(), barriers.size()});
}

vector<byte> ReadBuffer(Device& device, Buffer& buffer, BufferStates state, uint64_t size) {
    auto readback = CreateReadbackBuffer(device, size).Unwrap();
    auto recording = BeginRecording(device);
    auto& commands = *recording.Commands;
    Barrier(commands, {BarrierBufferDescriptor{.Target = &buffer, .Before = state, .After = BufferState::CopySource}});
    commands.CopyBufferToBuffer(readback.get(), 0, &buffer, 0, size);
    Barrier(commands, {BarrierBufferDescriptor{.Target = readback.get(), .Before = BufferState::CopyDestination, .After = BufferState::HostRead}});
    SubmitAndWait(device, recording);
    return MapAndRead(*readback);
}

vector<byte> ReadTexture(Device& device, Texture& texture, TextureStates state, uint32_t layer) {
    const auto desc = texture.GetDesc();
    const uint32_t texelSize = GetTextureFormatBytesPerPixel(desc.Format);
    const uint32_t pitch = CopyRowPitch(device, desc.Width, texelSize);
    auto readback = CreateReadbackBuffer(device, uint64_t{pitch} * desc.Height).Unwrap();
    auto recording = BeginRecording(device);
    auto& commands = *recording.Commands;
    const SubresourceRange range{layer, 1, 0, 1};
    Barrier(commands, {BarrierTextureDescriptor{.Target = &texture, .Before = state, .After = TextureState::CopySource, .Range = range}});
    commands.CopyTextureToBuffer(readback.get(), 0, &texture, range);
    Barrier(commands, {BarrierBufferDescriptor{.Target = readback.get(), .Before = BufferState::CopyDestination, .After = BufferState::HostRead}});
    SubmitAndWait(device, recording);
    const auto pitched = MapAndRead(*readback);
    vector<byte> texels;
    const size_t rowSize = size_t{desc.Width} * texelSize;
    for (uint32_t y = 0; y < desc.Height && !pitched.empty(); ++y) {
        const auto row = pitched.begin() + ptrdiff_t(size_t{y} * pitch);
        texels.insert(texels.end(), row, row + ptrdiff_t(rowSize));
    }
    return texels;
}

void UploadTexture(Device& device, Texture& texture, TextureStates state, std::span<const byte> texels) {
    const auto desc = texture.GetDesc();
    const uint32_t rowSize = desc.Width * GetTextureFormatBytesPerPixel(desc.Format);
    const uint32_t pitch = CopyRowPitch(device, desc.Width, GetTextureFormatBytesPerPixel(desc.Format));
    vector<byte> pitched(size_t{pitch} * desc.Height);
    for (uint32_t y = 0; y < desc.Height; ++y)
        std::memcpy(pitched.data() + size_t{y} * pitch, texels.data() + size_t{y} * rowSize, rowSize);
    auto upload = CreateUploadBuffer(device, pitched, BufferUse::CopySource).Unwrap();
    auto recording = BeginRecording(device);
    auto& commands = *recording.Commands;
    Barrier(commands, {BarrierBufferDescriptor{.Target = upload.get(), .Before = BufferState::HostWrite, .After = BufferState::CopySource},
                       BarrierTextureDescriptor{.Target = &texture, .Before = state, .After = TextureState::CopyDestination}});
    commands.CopyBufferToTexture(&texture, {0, 1, 0, 1}, upload.get(), 0);
    SubmitAndWait(device, recording);
}

}  // namespace radray::render::test
