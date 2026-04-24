#include <radray/runtime/gpu_system.h>

#include <algorithm>
#include <type_traits>
#include <variant>

#include <radray/logger.h>
#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

namespace radray {

class GpuResourceRegistry {
public:
    enum class Kind {
        Unknown,
        Buffer,
        Texture,
        TextureView,
        Sampler
    };

    enum class State {
        Alive,
        PendingDestroy
    };

    struct TextureParentRef {
        GpuResourceRegistry* Registry{nullptr};
        GpuTextureHandle Handle{};
        render::Texture* Texture{nullptr};
    };

    explicit GpuResourceRegistry(render::Device* device) noexcept;
    ~GpuResourceRegistry() noexcept;

    GpuBufferHandle CreateBuffer(const render::BufferDescriptor& desc);
    GpuTextureHandle CreateTexture(const render::TextureDescriptor& desc);
    GpuTextureViewHandle CreateTextureView(const GpuTextureViewDescriptor& desc, const TextureParentRef& parent);
    GpuSamplerHandle CreateSampler(const render::SamplerDescriptor& desc);

    render::Texture* FindAliveTexture(const GpuTextureHandle& handle) noexcept;
    const render::Texture* FindAliveTexture(const GpuTextureHandle& handle) const noexcept;

    bool Contains(const GpuResourceHandle& handle) const noexcept;
    bool IsPendingDestroy(const GpuResourceHandle& handle) const noexcept;
    Kind GetKind(const GpuResourceHandle& handle) const noexcept;
    void MarkPendingDestroy(const GpuResourceHandle& handle);

    void AddTextureChildViewRef(const GpuTextureHandle& handle);
    void ReleaseTextureChildViewRef(const GpuTextureHandle& handle) noexcept;

    bool TryDestroyImmediately(const GpuResourceHandle& handle) noexcept;
    bool TryRetire(const GpuResourceHandle& handle) noexcept;
    void Clear() noexcept;

private:
    struct BufferRecord {
        unique_ptr<render::Buffer> Buffer{};
    };

    struct TextureRecord {
        unique_ptr<render::Texture> Texture{};
        uint32_t ChildViewCount{0};
    };

    struct TextureViewRecord {
        unique_ptr<render::TextureView> TextureView{};
        TextureParentRef ParentTexture{};
    };

    struct SamplerRecord {
        unique_ptr<render::Sampler> Sampler{};
    };

    class ResourceRecord {
    public:
        using Payload = std::variant<BufferRecord, TextureRecord, TextureViewRecord, SamplerRecord>;

        Payload Data;
        void* NativeHandle{nullptr};
        State State{State::Alive};

        explicit ResourceRecord(BufferRecord bufferRecord) noexcept
            : Data(std::move(bufferRecord)),
              NativeHandle(std::get<BufferRecord>(Data).Buffer.get()) {}

        explicit ResourceRecord(TextureRecord textureRecord) noexcept
            : Data(std::move(textureRecord)),
              NativeHandle(std::get<TextureRecord>(Data).Texture.get()) {}

        explicit ResourceRecord(TextureViewRecord textureViewRecord) noexcept
            : Data(std::move(textureViewRecord)),
              NativeHandle(std::get<TextureViewRecord>(Data).TextureView.get()) {}

        explicit ResourceRecord(SamplerRecord samplerRecord) noexcept
            : Data(std::move(samplerRecord)),
              NativeHandle(std::get<SamplerRecord>(Data).Sampler.get()) {}

        Kind GetKind() const noexcept {
            return std::visit(
                [](const auto& record) noexcept {
                    using Record = std::decay_t<decltype(record)>;
                    if constexpr (std::is_same_v<Record, BufferRecord>) {
                        return Kind::Buffer;
                    } else if constexpr (std::is_same_v<Record, TextureRecord>) {
                        return Kind::Texture;
                    } else if constexpr (std::is_same_v<Record, TextureViewRecord>) {
                        return Kind::TextureView;
                    } else {
                        static_assert(std::is_same_v<Record, SamplerRecord>);
                        return Kind::Sampler;
                    }
                },
                Data);
        }
    };

    const ResourceRecord* FindRecord(const GpuResourceHandle& handle) const noexcept;
    ResourceRecord* FindRecord(const GpuResourceHandle& handle) noexcept;
    void EraseRecord(uint64_t handleId) noexcept;

    render::Device* _device{nullptr};
    uint64_t _nextHandle{1};
    unordered_map<uint64_t, ResourceRecord> _records{};
};

GpuTask::GpuTask(GpuRuntime* runtime, render::Fence* fence, uint64_t signalValue) noexcept
    : _runtime(runtime),
      _fence(fence),
      _signalValue(signalValue) {}

bool GpuTask::IsValid() const {
    return _fence != nullptr && _signalValue != 0;
}

bool GpuTask::IsCompleted() const {
    return !this->IsValid() || _fence->GetCompletedValue() >= _signalValue;
}

void GpuTask::Wait() {
    if (this->IsValid()) {
        _fence->Wait(_signalValue);
    }
}

GpuSurface::GpuSurface(
    GpuRuntime* runtime,
    unique_ptr<render::SwapChain> swapchain,
    uint32_t queueSlot) noexcept
    : _runtime(runtime),
      _swapchain(std::move(swapchain)),
      _queueSlot(queueSlot) {}

GpuSurface::~GpuSurface() noexcept {
    this->Destroy();
}

bool GpuSurface::IsValid() const {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        return _swapchain != nullptr;
    }
    return _swapchain != nullptr;
}

void GpuSurface::Destroy() {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        _swapchain.reset();
        return;
    }
    _swapchain.reset();
}

GpuResourceRegistry::GpuResourceRegistry(render::Device* device) noexcept
    : _device(device) {}

GpuResourceRegistry::~GpuResourceRegistry() noexcept {
    this->Clear();
}

GpuBufferHandle GpuResourceRegistry::CreateBuffer(const render::BufferDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto bufferOpt = _device->CreateBuffer(desc);
    if (!bufferOpt.HasValue()) {
        throw GpuSystemException("Device::CreateBuffer failed");
    }

    auto buffer = bufferOpt.Release();
    auto* native = buffer.get();
    const uint64_t handleId = _nextHandle++;

    _records.emplace(handleId, ResourceRecord{BufferRecord{std::move(buffer)}});
    return {handleId, native};
}

GpuTextureHandle GpuResourceRegistry::CreateTexture(const render::TextureDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto textureOpt = _device->CreateTexture(desc);
    if (!textureOpt.HasValue()) {
        throw GpuSystemException("Device::CreateTexture failed");
    }

    auto texture = textureOpt.Release();
    auto* native = texture.get();
    const uint64_t handleId = _nextHandle++;

    _records.emplace(handleId, ResourceRecord{TextureRecord{std::move(texture)}});
    return {handleId, native};
}

GpuTextureViewHandle GpuResourceRegistry::CreateTextureView(const GpuTextureViewDescriptor& desc, const TextureParentRef& parent) {
    RADRAY_ASSERT(_device != nullptr);
    RADRAY_ASSERT(parent.Registry != nullptr);
    RADRAY_ASSERT(parent.Handle.IsValid());
    RADRAY_ASSERT(parent.Texture != nullptr);

    render::TextureViewDescriptor nativeDesc{};
    nativeDesc.Target = parent.Texture;
    nativeDesc.Dim = desc.Dim;
    nativeDesc.Format = desc.Format;
    nativeDesc.Range = desc.Range;
    nativeDesc.Usage = desc.Usage;

    auto textureViewOpt = _device->CreateTextureView(nativeDesc);
    if (!textureViewOpt.HasValue()) {
        throw GpuSystemException("Device::CreateTextureView failed");
    }

    auto textureView = textureViewOpt.Release();
    auto* native = textureView.get();
    const uint64_t handleId = _nextHandle++;

    _records.emplace(
        handleId,
        ResourceRecord{TextureViewRecord{
            .TextureView = std::move(textureView),
            .ParentTexture = parent,
        }});
    parent.Registry->AddTextureChildViewRef(parent.Handle);
    return {handleId, native};
}

GpuSamplerHandle GpuResourceRegistry::CreateSampler(const render::SamplerDescriptor& desc) {
    RADRAY_ASSERT(_device != nullptr);
    auto samplerOpt = _device->CreateSampler(desc);
    if (!samplerOpt.HasValue()) {
        throw GpuSystemException("Device::CreateSampler failed");
    }

    auto sampler = samplerOpt.Release();
    auto* native = sampler.get();
    const uint64_t handleId = _nextHandle++;

    _records.emplace(handleId, ResourceRecord{SamplerRecord{std::move(sampler)}});
    return {handleId, native};
}

render::Texture* GpuResourceRegistry::FindAliveTexture(const GpuTextureHandle& handle) noexcept {
    auto* record = this->FindRecord(handle);
    if (record == nullptr || record->State != State::Alive) {
        return nullptr;
    }
    const auto* textureRecord = std::get_if<TextureRecord>(&record->Data);
    return textureRecord != nullptr ? textureRecord->Texture.get() : nullptr;
}

const render::Texture* GpuResourceRegistry::FindAliveTexture(const GpuTextureHandle& handle) const noexcept {
    const auto* record = this->FindRecord(handle);
    if (record == nullptr || record->State != State::Alive) {
        return nullptr;
    }
    const auto* textureRecord = std::get_if<TextureRecord>(&record->Data);
    return textureRecord != nullptr ? textureRecord->Texture.get() : nullptr;
}

bool GpuResourceRegistry::Contains(const GpuResourceHandle& handle) const noexcept {
    return this->FindRecord(handle) != nullptr;
}

bool GpuResourceRegistry::IsPendingDestroy(const GpuResourceHandle& handle) const noexcept {
    const auto* record = this->FindRecord(handle);
    return record != nullptr && record->State == State::PendingDestroy;
}

GpuResourceRegistry::Kind GpuResourceRegistry::GetKind(const GpuResourceHandle& handle) const noexcept {
    const auto* record = this->FindRecord(handle);
    return record != nullptr ? record->GetKind() : Kind::Unknown;
}

void GpuResourceRegistry::MarkPendingDestroy(const GpuResourceHandle& handle) {
    auto* record = this->FindRecord(handle);
    RADRAY_ASSERT(record != nullptr);
    record->State = State::PendingDestroy;
}

void GpuResourceRegistry::AddTextureChildViewRef(const GpuTextureHandle& handle) {
    auto* record = this->FindRecord(handle);
    auto* textureRecord = record != nullptr ? std::get_if<TextureRecord>(&record->Data) : nullptr;
    RADRAY_ASSERT(textureRecord != nullptr);
    ++textureRecord->ChildViewCount;
}

void GpuResourceRegistry::ReleaseTextureChildViewRef(const GpuTextureHandle& handle) noexcept {
    auto* record = this->FindRecord(handle);
    auto* textureRecord = record != nullptr ? std::get_if<TextureRecord>(&record->Data) : nullptr;
    if (textureRecord == nullptr) {
        return;
    }
    if (textureRecord->ChildViewCount > 0) {
        --textureRecord->ChildViewCount;
    }
}

bool GpuResourceRegistry::TryRetire(const GpuResourceHandle& handle) noexcept {
    auto* record = this->FindRecord(handle);
    if (record == nullptr) {
        return true;
    }
    if (record->State != State::PendingDestroy) {
        return false;
    }
    const auto* textureRecord = std::get_if<TextureRecord>(&record->Data);
    if (textureRecord != nullptr && textureRecord->ChildViewCount != 0) {
        return false;
    }

    this->EraseRecord(handle.Handle);
    return true;
}

bool GpuResourceRegistry::TryDestroyImmediately(const GpuResourceHandle& handle) noexcept {
    auto* record = this->FindRecord(handle);
    if (record == nullptr) {
        return false;
    }
    if (record->State != State::Alive) {
        return false;
    }
    const auto* textureRecord = std::get_if<TextureRecord>(&record->Data);
    if (textureRecord != nullptr && textureRecord->ChildViewCount != 0) {
        return false;
    }

    this->EraseRecord(handle.Handle);
    return true;
}

void GpuResourceRegistry::Clear() noexcept {
    vector<uint64_t> textureViewHandles{};
    vector<uint64_t> otherHandles{};
    textureViewHandles.reserve(_records.size());
    otherHandles.reserve(_records.size());

    for (const auto& [handleId, record] : _records) {
        if (std::holds_alternative<TextureViewRecord>(record.Data)) {
            textureViewHandles.emplace_back(handleId);
        } else {
            otherHandles.emplace_back(handleId);
        }
    }

    for (uint64_t handleId : textureViewHandles) {
        this->EraseRecord(handleId);
    }
    for (uint64_t handleId : otherHandles) {
        this->EraseRecord(handleId);
    }
    _records.clear();
}

const GpuResourceRegistry::ResourceRecord* GpuResourceRegistry::FindRecord(const GpuResourceHandle& handle) const noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const auto it = _records.find(handle.Handle);
    if (it == _records.end()) {
        return nullptr;
    }
    if (it->second.NativeHandle != handle.NativeHandle) {
        return nullptr;
    }
    return &it->second;
}

GpuResourceRegistry::ResourceRecord* GpuResourceRegistry::FindRecord(const GpuResourceHandle& handle) noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const auto it = _records.find(handle.Handle);
    if (it == _records.end()) {
        return nullptr;
    }
    if (it->second.NativeHandle != handle.NativeHandle) {
        return nullptr;
    }
    return &it->second;
}

void GpuResourceRegistry::EraseRecord(uint64_t handleId) noexcept {
    const auto it = _records.find(handleId);
    if (it == _records.end()) {
        return;
    }

    ResourceRecord record = std::move(it->second);
    _records.erase(it);

    std::visit(
        [](const auto& resourceRecord) noexcept {
            using Record = std::decay_t<decltype(resourceRecord)>;
            if constexpr (std::is_same_v<Record, TextureViewRecord>) {
                const auto& textureViewRecord = resourceRecord;
                if (textureViewRecord.ParentTexture.Registry != nullptr &&
                    textureViewRecord.ParentTexture.Handle.IsValid()) {
                    textureViewRecord.ParentTexture.Registry->ReleaseTextureChildViewRef(
                        textureViewRecord.ParentTexture.Handle);
                }
            }
        },
        record.Data);
}

render::SwapChainDescriptor GpuSurface::GetDesc() const {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        RADRAY_ASSERT(_swapchain != nullptr);
        return _swapchain->GetDesc();
    }
    RADRAY_ASSERT(_swapchain != nullptr);
    return _swapchain->GetDesc();
}

uint32_t GpuSurface::GetQueueSlot() const {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        return _queueSlot;
    }
    return _queueSlot;
}

size_t GpuSurface::GetNextFrameSlotIndex() const {
    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        return _nextFrameSlotIndex;
    }
    return _nextFrameSlotIndex;
}

uint32_t GpuSurface::GetWidth() const {
    return this->GetDesc().Width;
}

uint32_t GpuSurface::GetHeight() const {
    return this->GetDesc().Height;
}

render::TextureFormat GpuSurface::GetFormat() const {
    return this->GetDesc().Format;
}

render::PresentMode GpuSurface::GetPresentMode() const {
    return this->GetDesc().PresentMode;
}

uint32_t GpuSurface::GetFlightFrameCount() const {
    return this->GetDesc().FlightFrameCount;
}

GpuAsyncContext::~GpuAsyncContext() noexcept = default;

GpuAsyncContext::GpuAsyncContext(
    GpuRuntime* runtime,
    render::CommandQueue* queue,
    uint32_t queueSlot) noexcept
    : _runtime(runtime),
      _queue(queue),
      _queueSlot(queueSlot),
      _resourceRegistry(runtime != nullptr && runtime->_device != nullptr
                            ? make_unique<GpuResourceRegistry>(runtime->_device.get())
                            : nullptr) {}

bool GpuAsyncContext::IsEmpty() const {
    return _cmdBuffers.empty();
}

bool GpuAsyncContext::DependsOn(const GpuTask& task) {
    if (!task.IsValid()) {
        return false;
    }
    RADRAY_ASSERT(_runtime == task._runtime);
    _waitFences.emplace_back(task._fence);
    _waitValues.emplace_back(task._signalValue);
    return true;
}

render::CommandBuffer* GpuAsyncContext::CreateCommandBuffer() {
    auto cmdBufferOpt = _runtime->_device->CreateCommandBuffer(_queue);
    if (!cmdBufferOpt.HasValue()) {
        throw GpuSystemException("Device::CreateCommandBuffer failed");
    }
    auto& cmdBuffer = _cmdBuffers.emplace_back(cmdBufferOpt.Release());
    return cmdBuffer.get();
}

GpuBufferHandle GpuAsyncContext::CreateTransientBuffer(const render::BufferDescriptor& desc) {
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateBuffer(desc);
}

GpuTextureHandle GpuAsyncContext::CreateTransientTexture(const render::TextureDescriptor& desc) {
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateTexture(desc);
}

GpuTextureViewHandle GpuAsyncContext::CreateTransientTextureView(const GpuTextureViewDescriptor& desc) {
    RADRAY_ASSERT(_resourceRegistry != nullptr);

    GpuResourceRegistry::TextureParentRef parent{};
    if (auto* localTexture = _resourceRegistry->FindAliveTexture(desc.Target); localTexture != nullptr) {
        parent.Registry = _resourceRegistry.get();
        parent.Handle = desc.Target;
        parent.Texture = localTexture;
        return _resourceRegistry->CreateTextureView(desc, parent);
    }

    if (_runtime != nullptr) {
        std::lock_guard<std::mutex> lock{_runtime->_runtimeMutex};
        if (_runtime->_resourceRegistry != nullptr) {
            auto* persistentTexture = _runtime->_resourceRegistry->FindAliveTexture(desc.Target);
            if (persistentTexture != nullptr) {
                parent.Registry = _runtime->_resourceRegistry.get();
                parent.Handle = desc.Target;
                parent.Texture = persistentTexture;
                return _resourceRegistry->CreateTextureView(desc, parent);
            }
        }
    }

    RADRAY_ASSERT(parent.Registry != nullptr);
    RADRAY_ASSERT(parent.Handle.IsValid());
    RADRAY_ASSERT(parent.Texture != nullptr);
    return _resourceRegistry->CreateTextureView(desc, parent);
}

GpuFrameContext::GpuFrameContext(
    GpuRuntime* runtime,
    GpuSurface* surface,
    size_t frameSlotIndex,
    render::Texture* backBuffer,
    uint32_t backBufferIndex) noexcept
    : GpuAsyncContext(runtime, surface->_swapchain->GetDesc().PresentQueue, surface->_queueSlot),
      _surface(surface),
      _frameSlotIndex(frameSlotIndex),
      _backBuffer(backBuffer),
      _backBufferIndex(backBufferIndex) {}

GpuFrameContext::~GpuFrameContext() noexcept {
    RADRAY_ASSERT(_consumeState != ConsumeState::Acquired);
}

render::Texture* GpuFrameContext::GetBackBuffer() const {
    return _backBuffer;
}

uint32_t GpuFrameContext::GetBackBufferIndex() const {
    return _backBufferIndex;
}

GpuRuntime::GpuRuntime(
    shared_ptr<render::Device> device,
    unique_ptr<render::InstanceVulkan> vkInstance) noexcept
    : _device(std::move(device)),
      _vkInstance(std::move(vkInstance)),
      _resourceRegistry(_device != nullptr ? make_unique<GpuResourceRegistry>(_device.get()) : nullptr) {}

GpuRuntime::~GpuRuntime() noexcept {
    this->Destroy();
}

bool GpuRuntime::IsValid() const {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    return _device != nullptr;
}

render::Device* GpuRuntime::GetDevice() const {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    return _device.get();
}

void GpuRuntime::Destroy() noexcept {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    _pendings.clear();
    _resourceRetirements.clear();
    _resourceRegistry.reset();
    for (auto& fences : _queueFences) {
        fences.clear();
    }
    _device.reset();
    if (_vkInstance != nullptr) {
        render::DestroyVulkanInstance(std::move(_vkInstance));
    }
}

unique_ptr<GpuSurface> GpuRuntime::CreateSurface(const GpuSurfaceDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(desc.FlightFrameCount > 0);
    RADRAY_ASSERT(_device != nullptr);
    auto queueOpt = _device->GetCommandQueue(render::QueueType::Direct, desc.QueueSlot);
    if (!queueOpt.HasValue()) {
        throw GpuSystemException("Device::GetCommandQueue failed");
    }
    auto queue = queueOpt.Get();
    render::SwapChainDescriptor swapChainDesc{};
    swapChainDesc.PresentQueue = queue;
    swapChainDesc.NativeHandler = desc.NativeHandler;
    swapChainDesc.Width = desc.Width;
    swapChainDesc.Height = desc.Height;
    swapChainDesc.BackBufferCount = desc.BackBufferCount;
    swapChainDesc.Format = desc.Format;
    swapChainDesc.PresentMode = desc.PresentMode;
    swapChainDesc.FlightFrameCount = desc.FlightFrameCount;
    auto swapchainOpt = _device->CreateSwapChain(swapChainDesc);
    if (!swapchainOpt.HasValue()) {
        throw GpuSystemException("Device::CreateSwapChain failed");
    }
    auto result = make_unique<GpuSurface>(this, swapchainOpt.Release(), desc.QueueSlot);
    const uint32_t actualFlightFrameCount = result->_swapchain->GetDesc().FlightFrameCount;
    result->_frameSlots.reserve(actualFlightFrameCount);
    for (uint32_t i = 0; i < actualFlightFrameCount; i++) {
        result->_frameSlots.emplace_back();
    }
    return result;
}

GpuBufferHandle GpuRuntime::CreateBuffer(const render::BufferDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateBuffer(desc);
}

GpuTextureHandle GpuRuntime::CreateTexture(const render::TextureDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateTexture(desc);
}

GpuTextureViewHandle GpuRuntime::CreateTextureView(const GpuTextureViewDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);

    auto* texture = _resourceRegistry->FindAliveTexture(desc.Target);
    RADRAY_ASSERT(texture != nullptr);

    GpuResourceRegistry::TextureParentRef parent{};
    parent.Registry = _resourceRegistry.get();
    parent.Handle = desc.Target;
    parent.Texture = texture;
    return _resourceRegistry->CreateTextureView(desc, parent);
}

GpuSamplerHandle GpuRuntime::CreateSampler(const render::SamplerDescriptor& desc) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    return _resourceRegistry->CreateSampler(desc);
}

void GpuRuntime::DestroyResourceImmediate(GpuResourceHandle handle) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    RADRAY_ASSERT(_resourceRegistry->Contains(handle));
    RADRAY_ASSERT(!_resourceRegistry->IsPendingDestroy(handle));
    const bool destroyed = _resourceRegistry->TryDestroyImmediately(handle);
    RADRAY_ASSERT(destroyed);
}

void GpuRuntime::DestroyResourceAfter(GpuResourceHandle handle, const GpuTask& task) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_resourceRegistry != nullptr);
    RADRAY_ASSERT(_resourceRegistry->Contains(handle));
    RADRAY_ASSERT(!_resourceRegistry->IsPendingDestroy(handle));
    RADRAY_ASSERT(task.IsValid());
    RADRAY_ASSERT(task._runtime == this);

    _resourceRegistry->MarkPendingDestroy(handle);
    ResourceRetirement retirement{};
    retirement.Handle = handle;
    retirement.Fence = task._fence;
    retirement.SignalValue = task._signalValue;
    _resourceRetirements.emplace_back(std::move(retirement));
}

GpuRuntime::BeginFrameResult GpuRuntime::BeginFrame(GpuSurface* surface) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(surface != nullptr);
    RADRAY_ASSERT(surface->_swapchain != nullptr);
    RADRAY_ASSERT(surface->_runtime == this);
    auto* fence = this->GetQueueFenceUnlocked(render::QueueType::Direct, surface->_queueSlot);
    const size_t frameSlotIndex = surface->_nextFrameSlotIndex;
    auto& nowFrame = surface->_frameSlots[frameSlotIndex];
    fence->Wait(nowFrame._fenceValue);
    return this->AcquireSwapChainUnlocked(surface, std::numeric_limits<uint64_t>::max());
}

GpuRuntime::BeginFrameResult GpuRuntime::TryBeginFrame(GpuSurface* surface) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(surface != nullptr);
    RADRAY_ASSERT(surface->_swapchain != nullptr);
    RADRAY_ASSERT(surface->_runtime == this);
    auto* fence = this->GetQueueFenceUnlocked(render::QueueType::Direct, surface->_queueSlot);
    const size_t frameSlotIndex = surface->_nextFrameSlotIndex;
    auto& nowFrame = surface->_frameSlots[frameSlotIndex];
    if (fence->GetCompletedValue() < nowFrame._fenceValue) {
        return {nullptr, render::SwapChainStatus::RetryLater};
    }
    return this->AcquireSwapChainUnlocked(surface, 0);
}

unique_ptr<GpuAsyncContext> GpuRuntime::BeginAsync(render::QueueType type, uint32_t queueSlot) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_device != nullptr);
    auto queueOpt = _device->GetCommandQueue(type, queueSlot);
    if (!queueOpt.HasValue()) {
        throw GpuSystemException("Device::GetCommandQueue failed");
    }
    auto queue = queueOpt.Get();
    return make_unique<GpuAsyncContext>(this, queue, queueSlot);
}

GpuRuntime::SubmittedBatch GpuRuntime::SubmitContextUnlocked(
    GpuAsyncContext* context,
    Nullable<render::SwapChainSyncObject*> waitToExecute,
    Nullable<render::SwapChainSyncObject*> readyToPresent) {
    RADRAY_ASSERT(context != nullptr);
    RADRAY_ASSERT(!context->IsEmpty());

    vector<render::CommandBuffer*> submitCmds;
    submitCmds.reserve(context->_cmdBuffers.size());
    for (const auto& cmdBuffer : context->_cmdBuffers) {
        submitCmds.emplace_back(cmdBuffer.get());
    }

    auto* fence = this->GetQueueFenceUnlocked(context->_queue->GetQueueType(), context->_queueSlot);
    const uint64_t signalValue = fence->GetLastSignaledValue() + 1;
    render::Fence* signalFences[] = {fence};
    uint64_t signalValues[] = {signalValue};

    render::CommandQueueSubmitDescriptor submitDesc{};
    submitDesc.CmdBuffers = submitCmds;
    submitDesc.SignalFences = signalFences;
    submitDesc.SignalValues = signalValues;
    submitDesc.WaitFences = context->_waitFences;
    submitDesc.WaitValues = context->_waitValues;

    render::SwapChainSyncObject* swapChainWait[] = {nullptr};
    render::SwapChainSyncObject* swapChainPresent[] = {nullptr};
    if (waitToExecute != nullptr) {
        swapChainWait[0] = waitToExecute.Get();
        submitDesc.WaitToExecute = swapChainWait;
    }
    if (readyToPresent != nullptr) {
        swapChainPresent[0] = readyToPresent.Get();
        submitDesc.ReadyToPresent = swapChainPresent;
    }

    context->_queue->Submit(submitDesc);
    return SubmittedBatch{fence, signalValue};
}

void GpuRuntime::ValidateFrameContextForConsume(const GpuFrameContext* context) const {
    RADRAY_UNUSED(context);
    RADRAY_ASSERT(context != nullptr);
    RADRAY_ASSERT(context->GetType() == GpuAsyncContext::Kind::Frame);
    RADRAY_ASSERT(context->_runtime == this);
    RADRAY_ASSERT(context->_surface != nullptr);
    RADRAY_ASSERT(context->_surface->_swapchain != nullptr);
    RADRAY_ASSERT(context->_surface->_runtime == this);
}

GpuRuntime::SubmitFrameResult GpuRuntime::FinalizeFrameUnlocked(unique_ptr<GpuFrameContext> context) {
    this->ValidateFrameContextForConsume(context.get());
    auto* surface = context->_surface;
    const size_t frameSlotIndex = context->_frameSlotIndex;
    const auto submitted = this->SubmitContextUnlocked(context.get(), context->_waitToDraw, context->_readyToPresent);
    GpuTask task{this, submitted.Fence, submitted.SignalValue};
    RADRAY_ASSERT(context->_acquiredFrame.has_value());
    const auto present = surface->_swapchain->Present(std::move(context->_acquiredFrame.value()));
    context->_acquiredFrame.reset();
    surface->_frameSlots[frameSlotIndex]._fenceValue = submitted.SignalValue;
    _pendings.emplace_back(std::move(context), submitted.Fence, submitted.SignalValue);
    return {std::move(task), present};
}

GpuTask GpuRuntime::SubmitAsync(unique_ptr<GpuAsyncContext> context) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(context != nullptr);
    RADRAY_ASSERT(context->GetType() == GpuAsyncContext::Kind::Async);
    RADRAY_ASSERT(context->_runtime == this);
    const auto submitted = this->SubmitContextUnlocked(context.get(), nullptr, nullptr);
    _pendings.emplace_back(std::move(context), submitted.Fence, submitted.SignalValue);
    return GpuTask{this, submitted.Fence, submitted.SignalValue};
}

GpuRuntime::SubmitFrameResult GpuRuntime::SubmitFrame(unique_ptr<GpuFrameContext> context) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    this->ValidateFrameContextForConsume(context.get());
    context->_consumeState = GpuFrameContext::ConsumeState::Submitted;
    return this->FinalizeFrameUnlocked(std::move(context));
}

GpuRuntime::SubmitFrameResult GpuRuntime::AbandonFrame(unique_ptr<GpuFrameContext> context) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    this->ValidateFrameContextForConsume(context.get());
    context->_consumeState = GpuFrameContext::ConsumeState::Abandoned;
    context->_cmdBuffers.clear();
    context->_waitFences.clear();
    context->_waitValues.clear();

    auto* cmd = context->CreateCommandBuffer();
    cmd->Begin();
    if (context->_backBuffer != nullptr) {
        render::ResourceBarrierDescriptor toPresent = render::BarrierTextureDescriptor{
            .Target = context->_backBuffer,
            .Before = render::TextureState::Undefined,
            .After = render::TextureState::Present,
        };
        cmd->ResourceBarrier(std::span{&toPresent, 1});
    }
    cmd->End();

    return this->FinalizeFrameUnlocked(std::move(context));
}

void GpuRuntime::ProcessTasks() {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    this->ProcessTasksUnlocked();
}

void GpuRuntime::ProcessTasksUnlocked() {
    std::erase_if(_pendings, [](const Pending& pending) {
        return pending._fence != nullptr && pending._fence->GetCompletedValue() >= pending._signalValue;
    });

    if (_resourceRegistry == nullptr) {
        _resourceRetirements.clear();
    } else {
        std::erase_if(_resourceRetirements, [this](const ResourceRetirement& retirement) {
            if (!retirement.IsReady()) {
                return false;
            }
            const auto kind = _resourceRegistry->GetKind(retirement.Handle);
            if (kind == GpuResourceRegistry::Kind::Unknown) {
                return true;
            }
            if (kind == GpuResourceRegistry::Kind::Texture) {
                return false;
            }
            return _resourceRegistry->TryRetire(retirement.Handle);
        });
        std::erase_if(_resourceRetirements, [this](const ResourceRetirement& retirement) {
            if (!retirement.IsReady()) {
                return false;
            }
            const auto kind = _resourceRegistry->GetKind(retirement.Handle);
            if (kind == GpuResourceRegistry::Kind::Unknown) {
                return true;
            }
            if (kind != GpuResourceRegistry::Kind::Texture) {
                return false;
            }
            return _resourceRegistry->TryRetire(retirement.Handle);
        });
    }
}

void GpuRuntime::Wait(render::QueueType type, uint32_t queueSlot) {
    std::lock_guard<std::mutex> lock{_runtimeMutex};
    RADRAY_ASSERT(_device != nullptr);
    auto queueOpt = _device->GetCommandQueue(type, queueSlot);
    if (!queueOpt.HasValue()) {
        throw GpuSystemException("Device::GetCommandQueue failed");
    }
    auto queue = queueOpt.Get();
    queue->Wait();
    this->ProcessTasksUnlocked();
}

render::Fence* GpuRuntime::GetQueueFenceUnlocked(render::QueueType type, uint32_t slot) {
    RADRAY_ASSERT(_device != nullptr);
#ifdef RADRAY_ENABLE_D3D12
    if (_device->GetBackend() == render::RenderBackend::D3D12) {
        auto deviceD3D12 = render::d3d12::CastD3D12Object(_device.get());
        auto queueD3D12Opt = deviceD3D12->GetCommandQueue(type, slot);
        if (!queueD3D12Opt.HasValue()) {
            throw GpuSystemException("Device::GetCommandQueue failed");
        }
        auto queueD3D12 = render::d3d12::CastD3D12Object(queueD3D12Opt.Get());
        return queueD3D12->_fence.get();
    }
#endif
    auto& fences = _queueFences[(size_t)type];
    if (fences.size() <= slot) {
        fences.reserve(slot + 1);
        for (size_t i = fences.size(); i <= slot; i++) {
            fences.emplace_back(nullptr);
        }
    }
    auto& fence = fences[slot];
    if (!fence) {
        auto fenceOpt = _device->CreateFence();
        if (!fenceOpt.HasValue()) {
            throw GpuSystemException("Device::CreateFence failed");
        }
        fence = fenceOpt.Release();
    }
    return fence.get();
}

GpuRuntime::BeginFrameResult GpuRuntime::AcquireSwapChainUnlocked(GpuSurface* surface, uint64_t timeoutMs) {
    const size_t frameSlotIndex = surface->_nextFrameSlotIndex;
    auto acqResult = surface->_swapchain->AcquireNext(timeoutMs);
    switch (acqResult.Status) {
        case render::SwapChainStatus::Success: {
            auto& frame = acqResult.Frame.value();
            auto context = make_unique<GpuFrameContext>(
                this,
                surface,
                frameSlotIndex,
                frame.GetBackBuffer(),
                frame.GetBackBufferIndex());
            context->_type = GpuAsyncContext::Kind::Frame;
            context->_waitToDraw = frame.GetWaitToDraw();
            context->_readyToPresent = frame.GetReadyToPresent();
            context->_acquiredFrame = std::move(frame);
            surface->_nextFrameSlotIndex = (frameSlotIndex + 1) % surface->_frameSlots.size();
            return {std::move(context), render::SwapChainStatus::Success};
        }
        case render::SwapChainStatus::RetryLater:
        case render::SwapChainStatus::RequireRecreate:
            return {nullptr, acqResult.Status};
        case render::SwapChainStatus::Error:
        default: {
            throw GpuSystemException("SwapChain::AcquireNext failed");
        }
    }
}

bool GpuRuntime::ResourceRetirement::IsReady() const noexcept {
    return Fence != nullptr && SignalValue != 0 && Fence->GetCompletedValue() >= SignalValue;
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(const render::VulkanDeviceDescriptor& desc, render::VulkanInstanceDescriptor vkInsDesc) {
    auto insOpt = render::CreateVulkanInstance(vkInsDesc);
    if (!insOpt.HasValue()) {
        return nullptr;
    }
    auto deviceOpt = render::CreateDevice(desc);
    if (!deviceOpt.HasValue()) {
        return nullptr;
    }
    return make_unique<GpuRuntime>(deviceOpt.Release(), insOpt.Release());
}

Nullable<unique_ptr<GpuRuntime>> GpuRuntime::Create(const render::D3D12DeviceDescriptor& desc) {
    auto deviceOpt = render::CreateDevice(desc);
    if (!deviceOpt.HasValue()) {
        return nullptr;
    }
    return make_unique<GpuRuntime>(deviceOpt.Release(), nullptr);
}

}  // namespace radray
