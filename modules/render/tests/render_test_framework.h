#pragma once

#include <mutex>
#include <optional>
#include <span>

#include <radray/logger.h>
#include <radray/render/common.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/render/shader_compiler/spvc.h>

namespace radray::render::test {

enum class TestBackend {
    D3D12,
    Vulkan,
};

std::string_view format_as(TestBackend backend) noexcept;

vector<TestBackend> GetEnabledTestBackends() noexcept;

template <typename T>
constexpr T AlignUp(T value, T alignment) noexcept {
    if (alignment == 0) {
        return value;
    }
    return static_cast<T>(((value + alignment - 1) / alignment) * alignment);
}

template <typename T>
std::span<const byte> BytesOf(const T& value) noexcept {
    return std::as_bytes(std::span{&value, 1});
}

template <typename T>
std::span<const byte> BytesOf(std::span<const T> values) noexcept {
    return std::as_bytes(values);
}

string DescribeBytes(std::span<const byte> data, size_t maxBytes = 32);

class LogCollector {
public:
    static void Callback(LogLevel level, std::string_view message, void* userData);

    vector<string> GetErrors() const;

    void Clear() noexcept;

private:
    mutable std::mutex _mutex;
    vector<string> _errors;
};

class ScopedVulkanInstance {
public:
    ~ScopedVulkanInstance() noexcept;

    unique_ptr<InstanceVulkan>& Ref() noexcept;

    void Reset() noexcept;

private:
    unique_ptr<InstanceVulkan> _instance{};
};

struct ComputeProgram {
    vector<byte> Blob{};
    unique_ptr<Shader> ShaderObject{};
    unique_ptr<RootSignature> RootSignatureObject{};
    unique_ptr<ComputePipelineState> PipelineObject{};
};

class ComputeTestContext {
public:
    ~ComputeTestContext() noexcept;

    bool Initialize(TestBackend backend, string* reason) noexcept;

    void Reset() noexcept;

    TestBackend GetBackend() const noexcept;

    std::string_view GetBackendName() const noexcept;

    shared_ptr<Device> GetDevice() const noexcept;

    Device* GetDevicePtr() const noexcept;

    CommandQueue* GetQueue() const noexcept;

    DeviceDetail GetDeviceDetail() const noexcept;

    vector<string> GetCapturedErrors() const;

    string JoinCapturedErrors(size_t maxCount = 8) const;

    void ClearCapturedErrors() noexcept;

    Nullable<unique_ptr<CommandBuffer>> CreateCommandBuffer(string* reason) noexcept;

    Nullable<unique_ptr<Buffer>> CreateBuffer(const BufferDescriptor& desc, string* reason) noexcept;

    Nullable<unique_ptr<BufferView>> CreateBufferView(const BufferViewDescriptor& desc, string* reason) noexcept;

    Nullable<unique_ptr<Texture>> CreateTexture(const TextureDescriptor& desc, string* reason) noexcept;

    Nullable<unique_ptr<TextureView>> CreateTextureView(const TextureViewDescriptor& desc, string* reason) noexcept;

    Nullable<unique_ptr<Sampler>> CreateSampler(const SamplerDescriptor& desc, string* reason) noexcept;

    Nullable<unique_ptr<DescriptorSet>> CreateDescriptorSet(
        RootSignature* rootSig,
        DescriptorSetIndex setIndex,
        string* reason) noexcept;

    bool SubmitAndWait(CommandBuffer* cmd, string* reason) noexcept;

    bool WriteHostVisibleBuffer(Buffer* buffer, std::span<const byte> data, string* reason) const noexcept;

    std::optional<vector<byte>> ReadHostVisibleBuffer(Buffer* buffer, uint64_t size, string* reason) const noexcept;

    bool UploadBufferData(
        Buffer* buffer,
        std::span<const byte> data,
        BufferStates finalState,
        string* reason) noexcept;

    bool UploadTexture2D(Texture* texture, std::span<const byte> texels, string* reason) noexcept;

    std::optional<ComputeProgram> CreateComputeProgram(
        std::string_view source,
        std::string_view entryPoint,
        string* reason) noexcept;

    void AppendHostWriteBarrier(
        vector<ResourceBarrierDescriptor>& barriers,
        Buffer* buffer,
        BufferStates after) const noexcept;

    void AppendReadbackPreCopyBarrier(
        vector<ResourceBarrierDescriptor>& barriers,
        Buffer* buffer) const noexcept;

    void AppendReadbackPostCopyBarrier(
        vector<ResourceBarrierDescriptor>& barriers,
        Buffer* buffer) const noexcept;

private:
    TestBackend _backend{TestBackend::D3D12};
    LogCollector _logs{};
    ScopedVulkanInstance _vkInstance{};
    shared_ptr<Device> _device{};
    shared_ptr<Dxc> _dxc{};
    CommandQueue* _queue{nullptr};
};

}  // namespace radray::render::test
