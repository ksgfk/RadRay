#pragma once

#include <atomic>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>

#include <gtest/gtest.h>

#include <radray/types.h>
#include <radray/render/rhi.h>

// Shared GPU test fixture. Rules for GPU tests are in docs/guide/testing.md.
namespace radray::render::test {

inline constexpr RenderBackend AllBackends[]{RenderBackend::D3D12, RenderBackend::Vulkan};

const char* BackendParamName(const testing::TestParamInfo<RenderBackend>& info);

// Owns every device a test opens. Devices use native validation and RHI validation;
// a test fails when native validation reports an error or RHI validation reports a
// diagnostic that the test did not take with TakeRhiValidationErrors().
class GpuTest : public testing::Test {
protected:
    void TearDown() override;

    // Returns null after skipping when the backend is not built or has no usable adapter,
    // and after failing when the backend is listed in RADRAY_TEST_REQUIRED_BACKENDS or
    // initialization fails on an available adapter.
    Nullable<Device*> OpenDevice(RenderBackend backend, bool externalResource = false);

    uint32_t TakeRhiValidationErrors() noexcept { return _rhiErrors.exchange(0); }

private:
    enum class OpenStatus { Opened,
                            Unavailable,
                            Failed };

    OpenStatus CreateD3D12Device(bool externalResource, string& reason);
    OpenStatus CreateVulkanDevice(bool externalResource, string& reason);
    OpenStatus AddDevice(Nullable<shared_ptr<Device>> device, string& reason);

    static void OnNativeMessage(LogLevel level, std::string_view message, void* userData);
    static void OnRhiMessage(LogLevel level, std::string_view message, void* userData);

    std::atomic<uint32_t> _nativeErrors{0};
    std::atomic<uint32_t> _rhiErrors{0};
    shared_ptr<DXGIDiagnosticContext> _dxgiContext;
    unique_ptr<DXGIFactory> _dxgiFactory;
    Nullable<InstanceVulkan*> _vulkanInstance{nullptr};
    vector<shared_ptr<Device>> _devices;
};

// One device of the backend named by the test parameter.
class BackendTest : public GpuTest, public testing::WithParamInterface<RenderBackend> {
protected:
    void SetUp() override;

    Device& GetDevice() const noexcept { return *_device; }

private:
    Device* _device{nullptr};
};

CommandQueue& DirectQueue(Device& device);

// Loads RADRAY_TEST_SHADER_DIR/shader_bytecode/<source>.<entry>.<dxil|spirv>.bin.
Nullable<unique_ptr<Shader>> LoadShader(Device& device, std::string_view source, std::string_view entry, ShaderStages stages);

// Upload buffer with MapWrite usage, filled with data.
Nullable<unique_ptr<Buffer>> CreateUploadBuffer(Device& device, std::span<const byte> data, BufferUses usage);

Nullable<unique_ptr<Buffer>> CreateReadbackBuffer(Device& device, uint64_t size);

// Maps a ReadBack buffer whose GPU writes are complete and returns its content.
vector<byte> MapAndRead(Buffer& readback);

// Row pitch of buffer/texture copies for one row of a color texture.
uint32_t CopyRowPitch(const Device& device, uint32_t width, uint32_t bytesPerTexel) noexcept;

struct Recording {
    unique_ptr<CommandAllocator> Allocator;
    unique_ptr<CommandBuffer> Commands;
};

// Returns a command buffer on the direct queue that has already begun recording.
Recording BeginRecording(Device& device);

// Ends the recording, submits it to the direct queue and waits for completion.
void SubmitAndWait(Device& device, Recording& recording);

void Barrier(CommandBuffer& commands, std::initializer_list<ResourceBarrierDescriptor> barriers);

// Copies the buffer from the given state to a new readback buffer and waits.
// The buffer is left in CopySource.
vector<byte> ReadBuffer(Device& device, Buffer& buffer, BufferStates state, uint64_t size);

// Copies one layer of a single-mip 2D color texture and returns tightly packed rows.
// The texture is left in CopySource.
vector<byte> ReadTexture(Device& device, Texture& texture, TextureStates state, uint32_t layer = 0);

// Uploads tightly packed rows into the first mip and layer of a single-mip 2D color
// texture from the given state and waits. The texture is left in CopyDestination.
void UploadTexture(Device& device, Texture& texture, TextureStates state, std::span<const byte> texels);

template <class T>
std::span<const byte> AsBytes(const T& value) noexcept {
    return std::as_bytes(std::span{&value, 1});
}

template <class T>
vector<T> FromBytes(std::span<const byte> bytes) {
    vector<T> values(bytes.size() / sizeof(T));
    std::memcpy(values.data(), bytes.data(), values.size() * sizeof(T));
    return values;
}

}  // namespace radray::render::test
