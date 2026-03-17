#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <radray/render/common.h>

namespace radray::runtime_tests {

inline constexpr size_t QueueIndex(render::QueueType type) noexcept {
    return static_cast<size_t>(type);
}

struct SubmittedBatch {
    vector<render::CommandBuffer*> CommandBuffers{};
    vector<render::Fence*> SignalFences{};
    vector<uint64_t> SignalValues{};
    vector<render::Fence*> WaitFences{};
    vector<uint64_t> WaitValues{};
    vector<render::SwapChainSyncObject*> WaitToExecute{};
    vector<render::SwapChainSyncObject*> ReadyToPresent{};
};

class FakeSwapChainSyncObject final : public render::SwapChainSyncObject {
public:
    bool IsValid() const noexcept override {
        return _valid;
    }

    void Destroy() noexcept override {
        _valid = false;
    }

private:
    bool _valid{true};
};

class FakeTexture final : public render::Texture {
public:
    explicit FakeTexture(render::TextureDescriptor desc = {}) noexcept
        : _desc(desc) {}

    bool IsValid() const noexcept override {
        return _valid;
    }

    void Destroy() noexcept override {
        _valid = false;
    }

    void SetDebugName(std::string_view name) noexcept override {
        _debugName = string{name};
    }

    render::TextureDescriptor GetDesc() const noexcept override {
        return _desc;
    }

private:
    bool _valid{true};
    string _debugName{};
    render::TextureDescriptor _desc{};
};

class FakeFence final : public render::Fence {
public:
    bool IsValid() const noexcept override {
        return _valid;
    }

    void Destroy() noexcept override {
        _valid = false;
    }

    void SetDebugName(std::string_view name) noexcept override {
        _debugName = string{name};
    }

    uint64_t GetCompletedValue() const noexcept override {
        return CompletedValue;
    }

    void Wait() noexcept override {
        ++WaitCallCount;
        if (AdvanceOnWait) {
            CompletedValue = std::max(CompletedValue, WaitAdvanceValue);
        }
    }

    uint64_t CompletedValue{0};
    uint64_t WaitAdvanceValue{0};
    uint32_t WaitCallCount{0};
    bool AdvanceOnWait{false};

private:
    bool _valid{true};
    string _debugName{};
};

class FakeCommandBuffer final : public render::CommandBuffer {
public:
    FakeCommandBuffer(render::QueueType queueType, render::CommandQueue* queue) noexcept
        : QueueType(queueType),
          Queue(queue) {}

    bool IsValid() const noexcept override {
        return _valid;
    }

    void Destroy() noexcept override {
        _valid = false;
    }

    void SetDebugName(std::string_view name) noexcept override {
        _debugName = string{name};
    }

    void Begin() noexcept override {
        ++BeginCallCount;
    }

    void End() noexcept override {
        ++EndCallCount;
    }

    void ResourceBarrier(std::span<const render::ResourceBarrierDescriptor> barriers) noexcept override {
        LastBarrierCount = static_cast<uint32_t>(barriers.size());
    }

    Nullable<unique_ptr<render::GraphicsCommandEncoder>> BeginRenderPass(
        const render::RenderPassDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    void EndRenderPass(unique_ptr<render::GraphicsCommandEncoder> /*encoder*/) noexcept override {}

    Nullable<unique_ptr<render::ComputeCommandEncoder>> BeginComputePass() noexcept override {
        return nullptr;
    }

    void EndComputePass(unique_ptr<render::ComputeCommandEncoder> /*encoder*/) noexcept override {}

    Nullable<unique_ptr<render::RayTracingCommandEncoder>> BeginRayTracingPass() noexcept override {
        return nullptr;
    }

    void EndRayTracingPass(unique_ptr<render::RayTracingCommandEncoder> /*encoder*/) noexcept override {}

    void CopyBufferToBuffer(
        render::Buffer* /*dst*/,
        uint64_t /*dstOffset*/,
        render::Buffer* /*src*/,
        uint64_t /*srcOffset*/,
        uint64_t size) noexcept override {
        LastCopySize = size;
    }

    void CopyBufferToTexture(
        render::Texture* /*dst*/,
        render::SubresourceRange /*dstRange*/,
        render::Buffer* /*src*/,
        uint64_t /*srcOffset*/) noexcept override {}

    void CopyTextureToBuffer(
        render::Buffer* /*dst*/,
        uint64_t /*dstOffset*/,
        render::Texture* /*src*/,
        render::SubresourceRange /*srcRange*/) noexcept override {}

    render::QueueType QueueType{render::QueueType::Direct};
    render::CommandQueue* Queue{nullptr};
    uint32_t BeginCallCount{0};
    uint32_t EndCallCount{0};
    uint32_t LastBarrierCount{0};
    uint64_t LastCopySize{0};

private:
    bool _valid{true};
    string _debugName{};
};

class FakeCommandQueue final : public render::CommandQueue {
public:
    explicit FakeCommandQueue(render::QueueType type) noexcept
        : Type(type) {}

    bool IsValid() const noexcept override {
        return _valid;
    }

    void Destroy() noexcept override {
        _valid = false;
    }

    void Submit(const render::CommandQueueSubmitDescriptor& desc) noexcept override {
        SubmittedBatch batch{};
        batch.CommandBuffers.assign(desc.CmdBuffers.begin(), desc.CmdBuffers.end());
        batch.SignalFences.assign(desc.SignalFences.begin(), desc.SignalFences.end());
        batch.SignalValues.assign(desc.SignalValues.begin(), desc.SignalValues.end());
        batch.WaitFences.assign(desc.WaitFences.begin(), desc.WaitFences.end());
        batch.WaitValues.assign(desc.WaitValues.begin(), desc.WaitValues.end());
        batch.WaitToExecute.assign(desc.WaitToExecute.begin(), desc.WaitToExecute.end());
        batch.ReadyToPresent.assign(desc.ReadyToPresent.begin(), desc.ReadyToPresent.end());
        Submits.push_back(std::move(batch));

        if (AutoCompleteSignalFences) {
            for (size_t i = 0; i < desc.SignalFences.size() && i < desc.SignalValues.size(); ++i) {
                if (auto* fakeFence = dynamic_cast<FakeFence*>(desc.SignalFences[i])) {
                    fakeFence->CompletedValue = std::max(fakeFence->CompletedValue, desc.SignalValues[i]);
                }
            }
        }
    }

    void Wait() noexcept override {
        ++WaitCallCount;
    }

    render::QueueType Type{render::QueueType::Direct};
    vector<SubmittedBatch> Submits{};
    uint32_t WaitCallCount{0};
    bool AutoCompleteSignalFences{false};

private:
    bool _valid{true};
};

class FakeSwapChain final : public render::SwapChain {
public:
    explicit FakeSwapChain(render::SwapChainDescriptor desc) noexcept
        : _desc(desc) {
        const uint32_t backBufferCount = std::max(desc.BackBufferCount, 1u);
        _backBuffers.reserve(backBufferCount);
        for (uint32_t i = 0; i < backBufferCount; ++i) {
            render::TextureDescriptor textureDesc{};
            textureDesc.Dim = render::TextureDimension::Dim2D;
            textureDesc.Format = desc.Format;
            textureDesc.Width = desc.Width;
            textureDesc.Height = desc.Height;
            textureDesc.DepthOrArraySize = 1;
            textureDesc.MipLevels = 1;
            textureDesc.SampleCount = 1;
            _backBuffers.push_back(std::make_unique<FakeTexture>(textureDesc));
        }
    }

    bool IsValid() const noexcept override {
        return _valid;
    }

    void Destroy() noexcept override {
        _valid = false;
    }

    render::AcquireResult AcquireNext() noexcept override {
        ++AcquireCallCount;
        if (!_queuedAcquireResults.empty()) {
            auto result = _queuedAcquireResults.front();
            _queuedAcquireResults.pop_front();
            if (result.BackBuffer != nullptr) {
                for (uint32_t i = 0; i < _backBuffers.size(); ++i) {
                    if (_backBuffers[i].get() == result.BackBuffer.Get()) {
                        _currentBackBufferIndex = i;
                        break;
                    }
                }
            }
            return result;
        }

        return MakeAcquireResult(_currentBackBufferIndex, true);
    }

    void Present(render::SwapChainSyncObject* waitToPresent) noexcept override {
        ++PresentCallCount;
        PresentedWaits.push_back(waitToPresent);
    }

    Nullable<render::Texture*> GetCurrentBackBuffer() const noexcept override {
        if (_backBuffers.empty()) {
            return nullptr;
        }
        return _backBuffers[_currentBackBufferIndex].get();
    }

    uint32_t GetCurrentBackBufferIndex() const noexcept override {
        return _currentBackBufferIndex;
    }

    uint32_t GetBackBufferCount() const noexcept override {
        return static_cast<uint32_t>(_backBuffers.size());
    }

    render::SwapChainDescriptor GetDesc() const noexcept override {
        return _desc;
    }

    render::AcquireResult MakeAcquireResult(uint32_t backBufferIndex, bool withSyncObjects) {
        render::AcquireResult result{};
        if (backBufferIndex < _backBuffers.size()) {
            _currentBackBufferIndex = backBufferIndex;
            result.BackBuffer = _backBuffers[backBufferIndex].get();
        }
        if (withSyncObjects) {
            auto waitToDraw = std::make_unique<FakeSwapChainSyncObject>();
            auto readyToPresent = std::make_unique<FakeSwapChainSyncObject>();
            result.WaitToDraw = waitToDraw.get();
            result.ReadyToPresent = readyToPresent.get();
            _waitToDrawSyncs.push_back(std::move(waitToDraw));
            _readyToPresentSyncs.push_back(std::move(readyToPresent));
        }
        return result;
    }

    void EnqueueAcquireSuccess(uint32_t backBufferIndex, bool withSyncObjects = true) {
        _queuedAcquireResults.push_back(MakeAcquireResult(backBufferIndex, withSyncObjects));
    }

    void EnqueueAcquireUnavailable() {
        _queuedAcquireResults.push_back(render::AcquireResult{});
    }

    FakeTexture* GetBackBuffer(uint32_t index) const noexcept {
        if (index >= _backBuffers.size()) {
            return nullptr;
        }
        return _backBuffers[index].get();
    }

    uint32_t AcquireCallCount{0};
    uint32_t PresentCallCount{0};
    vector<render::SwapChainSyncObject*> PresentedWaits{};

private:
    bool _valid{true};
    render::SwapChainDescriptor _desc{};
    uint32_t _currentBackBufferIndex{0};
    vector<unique_ptr<FakeTexture>> _backBuffers{};
    vector<unique_ptr<FakeSwapChainSyncObject>> _waitToDrawSyncs{};
    vector<unique_ptr<FakeSwapChainSyncObject>> _readyToPresentSyncs{};
    std::deque<render::AcquireResult> _queuedAcquireResults{};
};

class FakeDevice final : public render::Device {
public:
    explicit FakeDevice(render::RenderBackend backend = render::RenderBackend::D3D12) noexcept
        : Backend(backend) {
        Detail.GpuName = "FakeDevice";
        Detail.TextureDataPitchAlignment = 1;
    }

    bool IsValid() const noexcept override {
        return _valid;
    }

    void Destroy() noexcept override {
        _valid = false;
    }

    render::RenderBackend GetBackend() noexcept override {
        return Backend;
    }

    render::DeviceDetail GetDetail() const noexcept override {
        return Detail;
    }

    Nullable<render::CommandQueue*> GetCommandQueue(render::QueueType type, uint32_t slot = 0) noexcept override {
        if (slot != 0) {
            return nullptr;
        }
        auto& queue = _queues[QueueIndex(type)];
        if (queue == nullptr) {
            return nullptr;
        }
        return queue.get();
    }

    Nullable<unique_ptr<render::CommandBuffer>> CreateCommandBuffer(render::CommandQueue* queue) noexcept override {
        CreateCommandBufferRequests.push_back(queue);
        if (!AllowCreateCommandBuffer || queue == nullptr) {
            return nullptr;
        }

        auto* fakeQueue = dynamic_cast<FakeCommandQueue*>(queue);
        const render::QueueType queueType = fakeQueue != nullptr ? fakeQueue->Type : render::QueueType::Direct;
        return std::make_unique<FakeCommandBuffer>(queueType, queue);
    }

    Nullable<unique_ptr<render::Fence>> CreateFence() noexcept override {
        if (!AllowCreateFence) {
            return nullptr;
        }
        auto fence = std::make_unique<FakeFence>();
        CreatedFences.push_back(fence.get());
        return fence;
    }

    Nullable<unique_ptr<render::SwapChain>> CreateSwapChain(const render::SwapChainDescriptor& desc) noexcept override {
        if (!AllowCreateSwapChain) {
            return nullptr;
        }
        auto swapChain = std::make_unique<FakeSwapChain>(desc);
        CreatedSwapChains.push_back(swapChain.get());
        return swapChain;
    }

    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::BufferView>> CreateBufferView(
        const render::BufferViewDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::Texture>> CreateTexture(const render::TextureDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::TextureView>> CreateTextureView(
        const render::TextureViewDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::Shader>> CreateShader(const render::ShaderDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::RootSignature>> CreateRootSignature(
        const render::RootSignatureDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::DescriptorSet>> CreateDescriptorSet(
        render::RootSignature* /*rootSig*/,
        render::DescriptorSetIndex /*set*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::GraphicsPipelineState>> CreateGraphicsPipelineState(
        const render::GraphicsPipelineStateDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::ComputePipelineState>> CreateComputePipelineState(
        const render::ComputePipelineStateDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::AccelerationStructure>> CreateAccelerationStructure(
        const render::AccelerationStructureDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::AccelerationStructureView>> CreateAccelerationStructureView(
        const render::AccelerationStructureViewDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::RayTracingPipelineState>> CreateRayTracingPipelineState(
        const render::RayTracingPipelineStateDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::ShaderBindingTable>> CreateShaderBindingTable(
        const render::ShaderBindingTableDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::Sampler>> CreateSampler(const render::SamplerDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    Nullable<unique_ptr<render::BindlessArray>> CreateBindlessArray(
        const render::BindlessArrayDescriptor& /*desc*/) noexcept override {
        return nullptr;
    }

    FakeCommandQueue* EnsureQueue(render::QueueType type) {
        auto& queue = _queues[QueueIndex(type)];
        if (queue == nullptr) {
            queue = std::make_unique<FakeCommandQueue>(type);
        }
        return queue.get();
    }

    render::RenderBackend Backend{render::RenderBackend::D3D12};
    render::DeviceDetail Detail{};
    bool AllowCreateCommandBuffer{true};
    bool AllowCreateFence{true};
    bool AllowCreateSwapChain{true};
    vector<render::CommandQueue*> CreateCommandBufferRequests{};
    vector<FakeFence*> CreatedFences{};
    vector<FakeSwapChain*> CreatedSwapChains{};

private:
    bool _valid{true};
    std::array<std::unique_ptr<FakeCommandQueue>, static_cast<size_t>(render::QueueType::MAX_COUNT)> _queues{};
};

}  // namespace radray::runtime_tests
