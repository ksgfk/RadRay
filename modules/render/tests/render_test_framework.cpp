#include "render_test_framework.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace radray::render::test {
namespace {

void _StoreReason(string* reason, std::string_view message) {
    if (reason != nullptr) {
        *reason = string{message};
    }
}

bool _CreateDeviceForBackend(
    TestBackend backend,
    LogCollector* logs,
    ScopedVulkanInstance& vkInstance,
    shared_ptr<Device>& device,
    string* reason) noexcept {
    switch (backend) {
        case TestBackend::D3D12: {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
            D3D12DeviceDescriptor desc{};
            desc.AdapterIndex = std::nullopt;
            desc.IsEnableDebugLayer = true;
            desc.IsEnableGpuBasedValid = true;
            desc.LogCallback = &LogCollector::Callback;
            desc.LogUserData = logs;
            auto deviceOpt = CreateDevice(desc);
            if (!deviceOpt.HasValue()) {
                _StoreReason(reason, "CreateDevice(D3D12) failed");
                return false;
            }
            device = deviceOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            _StoreReason(reason, "D3D12 backend is not enabled for this build");
            return false;
#endif
        }
        case TestBackend::Vulkan: {
#if defined(RADRAY_ENABLE_VULKAN)
            VulkanInstanceDescriptor instanceDesc{};
            instanceDesc.AppName = "RenderRuntimeBindingTests";
            instanceDesc.AppVersion = 1;
            instanceDesc.EngineName = "RadRay";
            instanceDesc.EngineVersion = 1;
            instanceDesc.IsEnableDebugLayer = true;
            instanceDesc.IsEnableGpuBasedValid = false;
            instanceDesc.LogCallback = &LogCollector::Callback;
            instanceDesc.LogUserData = logs;
            auto instanceOpt = CreateVulkanInstance(instanceDesc);
            if (!instanceOpt.HasValue()) {
                _StoreReason(reason, "CreateVulkanInstance failed");
                return false;
            }
            vkInstance.Ref() = instanceOpt.Release();

            VulkanCommandQueueDescriptor queueDesc{};
            queueDesc.Type = QueueType::Direct;
            queueDesc.Count = 1;
            VulkanDeviceDescriptor deviceDesc{};
            deviceDesc.PhysicalDeviceIndex = std::nullopt;
            deviceDesc.Queues = std::span{&queueDesc, 1};
            auto deviceOpt = CreateDevice(deviceDesc);
            if (!deviceOpt.HasValue()) {
                _StoreReason(reason, "CreateDevice(Vulkan) failed");
                vkInstance.Reset();
                return false;
            }
            device = deviceOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            _StoreReason(reason, "Vulkan backend is not enabled for this build");
            return false;
#endif
        }
        default: {
            _StoreReason(reason, fmt::format("Unsupported backend {}", backend));
            return false;
        }
    }
}

}  // namespace

std::string_view format_as(TestBackend backend) noexcept {
    switch (backend) {
        case TestBackend::D3D12: return "d3d12";
        case TestBackend::Vulkan: return "vulkan";
        default: return "unknown";
    }
}

vector<TestBackend> GetEnabledTestBackends() noexcept {
    vector<TestBackend> backends{};
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.push_back(TestBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN) && defined(RADRAY_ENABLE_SPIRV_CROSS)
    backends.push_back(TestBackend::Vulkan);
#endif
    return backends;
}

string DescribeBytes(std::span<const byte> data, size_t maxBytes) {
    const size_t count = std::min(maxBytes, data.size());
    fmt::memory_buffer out{};
    fmt::format_to(std::back_inserter(out), "{} bytes [", data.size());
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            fmt::format_to(std::back_inserter(out), " ");
        }
        fmt::format_to(
            std::back_inserter(out),
            "{:02X}",
            std::to_integer<unsigned int>(data[i]));
    }
    if (data.size() > count) {
        fmt::format_to(std::back_inserter(out), " ...");
    }
    fmt::format_to(std::back_inserter(out), "]");
    return fmt::to_string(out);
}

void LogCollector::Callback(LogLevel level, std::string_view message, void* userData) {
    auto* self = static_cast<LogCollector*>(userData);
    if (self == nullptr) {
        return;
    }
    if (level != LogLevel::Err && level != LogLevel::Critical) {
        return;
    }
    std::lock_guard<std::mutex> lock(self->_mutex);
    self->_errors.emplace_back(message);
}

vector<string> LogCollector::GetErrors() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _errors;
}

void LogCollector::Clear() noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    _errors.clear();
}

ScopedVulkanInstance::~ScopedVulkanInstance() noexcept {
    this->Reset();
}

unique_ptr<InstanceVulkan>& ScopedVulkanInstance::Ref() noexcept {
    return _instance;
}

void ScopedVulkanInstance::Reset() noexcept {
    if (_instance != nullptr) {
        DestroyVulkanInstance(std::move(_instance));
    }
}

ComputeTestContext::~ComputeTestContext() noexcept {
    this->Reset();
}

bool ComputeTestContext::Initialize(TestBackend backend, string* reason) noexcept {
    this->Reset();
    _backend = backend;
    SetLogCallback(&LogCollector::Callback, &_logs);

#if defined(RADRAY_ENABLE_DXC)
    auto dxcOpt = CreateDxc();
    if (!dxcOpt.HasValue()) {
        _StoreReason(reason, "CreateDxc failed");
        return false;
    }
    _dxc = dxcOpt.Release();
#else
    _StoreReason(reason, "DXC is not enabled for this build");
    return false;
#endif

    if (!_CreateDeviceForBackend(backend, &_logs, _vkInstance, _device, reason)) {
        return false;
    }
    auto queueOpt = _device->GetCommandQueue(QueueType::Direct, 0);
    if (!queueOpt.HasValue()) {
        _StoreReason(reason, fmt::format("No direct queue available for {}", backend));
        this->Reset();
        return false;
    }
    _queue = queueOpt.Get();
    return true;
}

void ComputeTestContext::Reset() noexcept {
    if (_queue != nullptr && _device != nullptr) {
        _queue->Wait();
    }
    ClearLogCallback();
    _queue = nullptr;
    _device.reset();
    _vkInstance.Reset();
    _dxc.reset();
    _logs.Clear();
}

TestBackend ComputeTestContext::GetBackend() const noexcept {
    return _backend;
}

std::string_view ComputeTestContext::GetBackendName() const noexcept {
    return format_as(_backend);
}

shared_ptr<Device> ComputeTestContext::GetDevice() const noexcept {
    return _device;
}

Device* ComputeTestContext::GetDevicePtr() const noexcept {
    return _device.get();
}

CommandQueue* ComputeTestContext::GetQueue() const noexcept {
    return _queue;
}

DeviceDetail ComputeTestContext::GetDeviceDetail() const noexcept {
    return _device != nullptr ? _device->GetDetail() : DeviceDetail{};
}

vector<string> ComputeTestContext::GetCapturedErrors() const {
    return _logs.GetErrors();
}

string ComputeTestContext::JoinCapturedErrors(size_t maxCount) const {
    const auto errors = this->GetCapturedErrors();
    if (errors.empty()) {
        return {};
    }
    const size_t count = std::min(maxCount, errors.size());
    string result = fmt::format("{}", fmt::join(errors.begin(), errors.begin() + count, "\n"));
    if (errors.size() > count) {
        result = fmt::format("{}\n...({} more)", result, errors.size() - count);
    }
    return result;
}

void ComputeTestContext::ClearCapturedErrors() noexcept {
    _logs.Clear();
}

Nullable<unique_ptr<CommandBuffer>> ComputeTestContext::CreateCommandBuffer(string* reason) noexcept {
    if (_device == nullptr || _queue == nullptr) {
        _StoreReason(reason, "device or queue is not initialized");
        return nullptr;
    }
    auto cmdOpt = _device->CreateCommandBuffer(_queue);
    if (!cmdOpt.HasValue()) {
        _StoreReason(reason, fmt::format("CreateCommandBuffer failed on {}", this->GetBackendName()));
        return nullptr;
    }
    return cmdOpt;
}

Nullable<unique_ptr<Buffer>> ComputeTestContext::CreateBuffer(const BufferDescriptor& desc, string* reason) noexcept {
    if (_device == nullptr) {
        _StoreReason(reason, "device is not initialized");
        return nullptr;
    }
    auto bufferOpt = _device->CreateBuffer(desc);
    if (!bufferOpt.HasValue()) {
        _StoreReason(
            reason,
            fmt::format(
                "CreateBuffer failed on {} (size={}, memory={})",
                this->GetBackendName(),
                desc.Size,
                static_cast<uint32_t>(desc.Memory)));
        return nullptr;
    }
    return bufferOpt;
}

Nullable<unique_ptr<BufferView>> ComputeTestContext::CreateBufferView(
    const BufferViewDescriptor& desc,
    string* reason) noexcept {
    if (_device == nullptr) {
        _StoreReason(reason, "device is not initialized");
        return nullptr;
    }
    auto viewOpt = _device->CreateBufferView(desc);
    if (!viewOpt.HasValue()) {
        _StoreReason(
            reason,
            fmt::format(
                "CreateBufferView failed on {} (offset={}, size={}, stride={}, usage={})",
                this->GetBackendName(),
                desc.Range.Offset,
                desc.Range.Size,
                desc.Stride,
                desc.Usage));
        return nullptr;
    }
    return viewOpt;
}

Nullable<unique_ptr<Texture>> ComputeTestContext::CreateTexture(
    const TextureDescriptor& desc,
    string* reason) noexcept {
    if (_device == nullptr) {
        _StoreReason(reason, "device is not initialized");
        return nullptr;
    }
    auto textureOpt = _device->CreateTexture(desc);
    if (!textureOpt.HasValue()) {
        _StoreReason(
            reason,
            fmt::format(
                "CreateTexture failed on {} ({}x{}, format={})",
                this->GetBackendName(),
                desc.Width,
                desc.Height,
                desc.Format));
        return nullptr;
    }
    return textureOpt;
}

Nullable<unique_ptr<TextureView>> ComputeTestContext::CreateTextureView(
    const TextureViewDescriptor& desc,
    string* reason) noexcept {
    if (_device == nullptr) {
        _StoreReason(reason, "device is not initialized");
        return nullptr;
    }
    auto viewOpt = _device->CreateTextureView(desc);
    if (!viewOpt.HasValue()) {
        _StoreReason(
            reason,
            fmt::format(
                "CreateTextureView failed on {} (format={}, usage={})",
                this->GetBackendName(),
                desc.Format,
                desc.Usage));
        return nullptr;
    }
    return viewOpt;
}

Nullable<unique_ptr<Sampler>> ComputeTestContext::CreateSampler(
    const SamplerDescriptor& desc,
    string* reason) noexcept {
    if (_device == nullptr) {
        _StoreReason(reason, "device is not initialized");
        return nullptr;
    }
    auto samplerOpt = _device->CreateSampler(desc);
    if (!samplerOpt.HasValue()) {
        _StoreReason(
            reason,
            fmt::format(
                "CreateSampler failed on {} (filters={}/{}/{})",
                this->GetBackendName(),
                static_cast<uint32_t>(desc.MinFilter),
                static_cast<uint32_t>(desc.MagFilter),
                static_cast<uint32_t>(desc.MipmapFilter)));
        return nullptr;
    }
    return samplerOpt;
}

Nullable<unique_ptr<DescriptorSet>> ComputeTestContext::CreateDescriptorSet(
    RootSignature* rootSig,
    DescriptorSetIndex setIndex,
    string* reason) noexcept {
    if (_device == nullptr) {
        _StoreReason(reason, "device is not initialized");
        return nullptr;
    }
    auto setOpt = _device->CreateDescriptorSet(rootSig, setIndex);
    if (!setOpt.HasValue()) {
        _StoreReason(
            reason,
            fmt::format(
                "CreateDescriptorSet failed on {} (set={})",
                this->GetBackendName(),
                setIndex.Value));
        return nullptr;
    }
    return setOpt;
}

bool ComputeTestContext::SubmitAndWait(CommandBuffer* cmd, string* reason) noexcept {
    if (_queue == nullptr) {
        _StoreReason(reason, "queue is not initialized");
        return false;
    }
    if (cmd == nullptr) {
        _StoreReason(reason, "command buffer is null");
        return false;
    }
    CommandBuffer* cmds[] = {cmd};
    CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = cmds;
    _queue->Submit(submitDesc);
    _queue->Wait();
    return true;
}

bool ComputeTestContext::WriteHostVisibleBuffer(
    Buffer* buffer,
    std::span<const byte> data,
    string* reason) const noexcept {
    if (buffer == nullptr) {
        _StoreReason(reason, "buffer is null");
        return false;
    }
    if (data.empty()) {
        return true;
    }
    void* mapped = buffer->Map(0, data.size());
    if (mapped == nullptr) {
        _StoreReason(
            reason,
            fmt::format(
                "Map failed on {} for {} bytes",
                this->GetBackendName(),
                data.size()));
        return false;
    }
    std::memcpy(mapped, data.data(), data.size());
    buffer->Unmap(0, data.size());
    return true;
}

std::optional<vector<byte>> ComputeTestContext::ReadHostVisibleBuffer(
    Buffer* buffer,
    uint64_t size,
    string* reason) const noexcept {
    if (buffer == nullptr) {
        _StoreReason(reason, "buffer is null");
        return std::nullopt;
    }
    vector<byte> result(size);
    if (size == 0) {
        return result;
    }
    void* mapped = buffer->Map(0, size);
    if (mapped == nullptr) {
        _StoreReason(
            reason,
            fmt::format(
                "Map for readback failed on {} for {} bytes",
                this->GetBackendName(),
                size));
        return std::nullopt;
    }
    std::memcpy(result.data(), mapped, size);
    buffer->Unmap(0, size);
    return result;
}

bool ComputeTestContext::UploadBufferData(
    Buffer* buffer,
    std::span<const byte> data,
    BufferStates finalState,
    string* reason) noexcept {
    if (buffer == nullptr) {
        _StoreReason(reason, "buffer is null");
        return false;
    }
    if (data.empty()) {
        return true;
    }
    const auto desc = buffer->GetDesc();
    if (data.size() > desc.Size) {
        _StoreReason(
            reason,
            fmt::format(
                "UploadBufferData size exceeds target buffer capacity, size={}, capacity={}",
                data.size(),
                desc.Size));
        return false;
    }

    BufferDescriptor uploadDesc{};
    uploadDesc.Size = data.size();
    uploadDesc.Memory = MemoryType::Upload;
    uploadDesc.Usage = BufferUse::MapWrite | BufferUse::CopySource;
    auto uploadOpt = this->CreateBuffer(uploadDesc, reason);
    if (!uploadOpt.HasValue()) {
        return false;
    }
    auto upload = uploadOpt.Release();
    if (!this->WriteHostVisibleBuffer(upload.get(), data, reason)) {
        return false;
    }

    auto cmdOpt = this->CreateCommandBuffer(reason);
    if (!cmdOpt.HasValue()) {
        return false;
    }
    auto cmd = cmdOpt.Release();
    cmd->Begin();

    vector<ResourceBarrierDescriptor> preCopy{};
    this->AppendHostWriteBarrier(preCopy, upload.get(), BufferState::CopySource);
    preCopy.push_back(BarrierBufferDescriptor{
        .Target = buffer,
        .Before = BufferState::Common,
        .After = BufferState::CopyDestination,
    });
    cmd->ResourceBarrier(preCopy);
    cmd->CopyBufferToBuffer(buffer, 0, upload.get(), 0, data.size());

    if (finalState != BufferState::CopyDestination) {
        ResourceBarrierDescriptor postCopy = BarrierBufferDescriptor{
            .Target = buffer,
            .Before = BufferState::CopyDestination,
            .After = finalState,
        };
        cmd->ResourceBarrier(std::span{&postCopy, 1});
    }
    cmd->End();
    return this->SubmitAndWait(cmd.get(), reason);
}

bool ComputeTestContext::UploadTexture2D(
    Texture* texture,
    std::span<const byte> texels,
    string* reason) noexcept {
    if (texture == nullptr) {
        _StoreReason(reason, "texture is null");
        return false;
    }
    const auto desc = texture->GetDesc();
    if (desc.Dim != TextureDimension::Dim2D ||
        desc.MipLevels != 1 ||
        desc.DepthOrArraySize != 1 ||
        desc.SampleCount != 1) {
        _StoreReason(
            reason,
            fmt::format(
                "UploadTexture2D only supports 2D single-mip single-layer textures (dim={}, mips={}, layers={}, samples={})",
                desc.Dim,
                desc.MipLevels,
                desc.DepthOrArraySize,
                desc.SampleCount));
        return false;
    }
    const uint32_t bytesPerPixel = GetTextureFormatBytesPerPixel(desc.Format);
    if (bytesPerPixel == 0) {
        _StoreReason(reason, fmt::format("Unsupported texture format {}", desc.Format));
        return false;
    }
    const uint64_t expectedBytes = static_cast<uint64_t>(desc.Width) * desc.Height * bytesPerPixel;
    if (texels.size() != expectedBytes) {
        _StoreReason(
            reason,
            fmt::format(
                "Texture upload byte size mismatch, expected {}, actual {}",
                expectedBytes,
                texels.size()));
        return false;
    }

    const uint64_t rowAlignment = std::max<uint64_t>(1, this->GetDeviceDetail().TextureDataPitchAlignment);
    const uint64_t bytesPerRow = static_cast<uint64_t>(desc.Width) * bytesPerPixel;
    const uint64_t alignedBytesPerRow = AlignUp(bytesPerRow, rowAlignment);
    vector<byte> staging(alignedBytesPerRow * desc.Height, byte{0});
    for (uint32_t row = 0; row < desc.Height; ++row) {
        const auto* src = texels.data() + row * bytesPerRow;
        auto* dst = staging.data() + row * alignedBytesPerRow;
        std::memcpy(dst, src, bytesPerRow);
    }

    BufferDescriptor uploadDesc{};
    uploadDesc.Size = staging.size();
    uploadDesc.Memory = MemoryType::Upload;
    uploadDesc.Usage = BufferUse::MapWrite | BufferUse::CopySource;
    auto uploadOpt = this->CreateBuffer(uploadDesc, reason);
    if (!uploadOpt.HasValue()) {
        return false;
    }
    auto upload = uploadOpt.Release();
    if (!this->WriteHostVisibleBuffer(upload.get(), staging, reason)) {
        return false;
    }

    auto cmdOpt = this->CreateCommandBuffer(reason);
    if (!cmdOpt.HasValue()) {
        return false;
    }
    auto cmd = cmdOpt.Release();
    cmd->Begin();
    vector<ResourceBarrierDescriptor> beforeCopy{};
    this->AppendHostWriteBarrier(beforeCopy, upload.get(), BufferState::CopySource);
    beforeCopy.push_back(BarrierTextureDescriptor{
        .Target = texture,
        .Before = TextureState::Undefined,
        .After = TextureState::CopyDestination,
    });
    cmd->ResourceBarrier(beforeCopy);
    cmd->CopyBufferToTexture(texture, SubresourceRange{0, 1, 0, 1}, upload.get(), 0);
    ResourceBarrierDescriptor afterCopy = BarrierTextureDescriptor{
        .Target = texture,
        .Before = TextureState::CopyDestination,
        .After = TextureState::ShaderRead,
    };
    cmd->ResourceBarrier(std::span{&afterCopy, 1});
    cmd->End();
    return this->SubmitAndWait(cmd.get(), reason);
}

std::optional<ComputeProgram> ComputeTestContext::CreateComputeProgram(
    std::string_view source,
    std::string_view entryPoint,
    string* reason) noexcept {
    if (_device == nullptr || _dxc == nullptr) {
        _StoreReason(reason, "device or dxc is not initialized");
        return std::nullopt;
    }

    DxcCompileParams params{};
    params.Code = source;
    params.EntryPoint = entryPoint;
    params.Stage = ShaderStage::Compute;
    params.SM = HlslShaderModel::SM60;
    params.IsOptimize = false;
    params.IsSpirv = _backend == TestBackend::Vulkan;
    params.EnableUnbounded = false;
    auto outputOpt = _dxc->Compile(params);
    if (!outputOpt.has_value()) {
        _StoreReason(
            reason,
            fmt::format(
                "DXC compile failed for {} compute shader entry '{}'",
                this->GetBackendName(),
                entryPoint));
        return std::nullopt;
    }
    auto output = std::move(outputOpt.value());

    ShaderReflectionDesc reflection{};
    ShaderBlobCategory category = ShaderBlobCategory::DXIL;
    if (_backend == TestBackend::D3D12) {
        auto reflectionOpt = _dxc->GetShaderDescFromOutput(output.Refl);
        if (!reflectionOpt.has_value()) {
            _StoreReason(reason, "DXIL reflection extraction failed");
            return std::nullopt;
        }
        reflection = std::move(reflectionOpt.value());
        category = ShaderBlobCategory::DXIL;
    } else {
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
        auto reflectionOpt = ReflectSpirv(SpirvBytecodeView{
            .Data = output.Data,
            .EntryPointName = entryPoint,
            .Stage = ShaderStage::Compute,
        });
        if (!reflectionOpt.has_value()) {
            _StoreReason(reason, "SPIR-V reflection extraction failed");
            return std::nullopt;
        }
        reflection = std::move(reflectionOpt.value());
        category = ShaderBlobCategory::SPIRV;
#else
        _StoreReason(reason, "SPIR-V Cross reflection is not enabled for this build");
        return std::nullopt;
#endif
    }

    ComputeProgram program{};
    program.Blob = std::move(output.Data);

    ShaderDescriptor shaderDesc{};
    shaderDesc.Source = std::span<const byte>{program.Blob.data(), program.Blob.size()};
    shaderDesc.Category = category;
    shaderDesc.Stages = ShaderStage::Compute;
    shaderDesc.Reflection = std::move(reflection);
    auto shaderOpt = _device->CreateShader(shaderDesc);
    if (!shaderOpt.HasValue()) {
        _StoreReason(
            reason,
            fmt::format(
                "CreateShader failed for {} compute shader '{}'",
                this->GetBackendName(),
                entryPoint));
        return std::nullopt;
    }
    program.ShaderObject = shaderOpt.Release();

    Shader* shaders[] = {program.ShaderObject.get()};
    RootSignatureDescriptor rootSignatureDesc{};
    rootSignatureDesc.Shaders = shaders;
    auto rootSigOpt = _device->CreateRootSignature(rootSignatureDesc);
    if (!rootSigOpt.HasValue()) {
        _StoreReason(
            reason,
            fmt::format(
                "CreateRootSignature failed for {} compute shader '{}'",
                this->GetBackendName(),
                entryPoint));
        return std::nullopt;
    }
    program.RootSignatureObject = rootSigOpt.Release();

    ComputePipelineStateDescriptor psoDesc{};
    psoDesc.RootSig = program.RootSignatureObject.get();
    psoDesc.CS = ShaderEntry{
        .Target = program.ShaderObject.get(),
        .EntryPoint = entryPoint,
    };
    auto psoOpt = _device->CreateComputePipelineState(psoDesc);
    if (!psoOpt.HasValue()) {
        _StoreReason(
            reason,
            fmt::format(
                "CreateComputePipelineState failed for {} compute shader '{}'",
                this->GetBackendName(),
                entryPoint));
        return std::nullopt;
    }
    program.PipelineObject = psoOpt.Release();
    return program;
}

void ComputeTestContext::AppendHostWriteBarrier(
    vector<ResourceBarrierDescriptor>& barriers,
    Buffer* buffer,
    BufferStates after) const noexcept {
    if (_backend != TestBackend::Vulkan || buffer == nullptr) {
        return;
    }
    barriers.push_back(BarrierBufferDescriptor{
        .Target = buffer,
        .Before = BufferState::HostWrite,
        .After = after,
    });
}

void ComputeTestContext::AppendReadbackPreCopyBarrier(
    vector<ResourceBarrierDescriptor>& barriers,
    Buffer* buffer) const noexcept {
    if (_backend != TestBackend::Vulkan || buffer == nullptr) {
        return;
    }
    barriers.push_back(BarrierBufferDescriptor{
        .Target = buffer,
        .Before = BufferState::Common,
        .After = BufferState::CopyDestination,
    });
}

void ComputeTestContext::AppendReadbackPostCopyBarrier(
    vector<ResourceBarrierDescriptor>& barriers,
    Buffer* buffer) const noexcept {
    if (_backend != TestBackend::Vulkan || buffer == nullptr) {
        return;
    }
    barriers.push_back(BarrierBufferDescriptor{
        .Target = buffer,
        .Before = BufferState::CopyDestination,
        .After = BufferState::HostRead,
    });
}

}  // namespace radray::render::test
