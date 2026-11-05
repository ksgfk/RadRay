#pragma once

#include <limits>

#include <radray/render/rhi.h>

namespace radray::render {

enum class ExternalMemoryHandleType : uint32_t {
    D3D12Resource,
    OpaqueWin32
};

enum class ExternalFenceHandleType : uint32_t {
    D3D12Fence,
    OpaqueWin32
};

enum class ExternalNativeApi : uint32_t {
    None,
    Win32,
    D3D12,
    Vulkan
};

struct ExternalError {
    string Message;
    ExternalNativeApi NativeApi{ExternalNativeApi::None};
    int64_t NativeCode{0};
};

template <class T>
using ExternalResult = std::variant<T, ExternalError>;

struct BorrowedExternalHandle {
    uintptr_t Value{0};
};

class OwnedExternalHandle {
public:
    OwnedExternalHandle() noexcept = default;
    explicit OwnedExternalHandle(uintptr_t value) noexcept;
    ~OwnedExternalHandle() noexcept;
    OwnedExternalHandle(const OwnedExternalHandle&) = delete;
    OwnedExternalHandle& operator=(const OwnedExternalHandle&) = delete;
    OwnedExternalHandle(OwnedExternalHandle&& other) noexcept;
    OwnedExternalHandle& operator=(OwnedExternalHandle&& other) noexcept;
    BorrowedExternalHandle Borrow() const noexcept { return {_value}; }
    bool IsValid() const noexcept { return _value != 0 && _value != std::numeric_limits<uintptr_t>::max(); }
    void Reset() noexcept;
    ExternalResult<OwnedExternalHandle> Duplicate() const noexcept;

private:
    uintptr_t _value{0};
};

struct ExternalDeviceIdentity {
    array<uint8_t, 8> Luid{};
    uint32_t NodeMask{0};
    friend bool operator==(const ExternalDeviceIdentity&, const ExternalDeviceIdentity&) noexcept = default;
};

struct ExternalVulkanIdentity {
    array<uint8_t, 16> DeviceUuid{};
    array<uint8_t, 16> DriverUuid{};
    friend bool operator==(const ExternalVulkanIdentity&, const ExternalVulkanIdentity&) noexcept = default;
};

struct ExternalVulkanMemoryInfo {
    ExternalVulkanIdentity Device;
    uint32_t MemoryTypeIndex{std::numeric_limits<uint32_t>::max()};
};

struct ExternalResourceDescriptor {
    ExternalMemoryHandleType HandleType{ExternalMemoryHandleType::D3D12Resource};
};

struct ExternalTextureDescriptor {
    ExternalMemoryHandleType HandleType{ExternalMemoryHandleType::D3D12Resource};
    bool AllowFormatReinterpretation{false};
};

struct ExternalFenceDescriptor {
    ExternalFenceHandleType HandleType{ExternalFenceHandleType::D3D12Fence};
    uint64_t InitialValue{0};
};

struct ExternalSupport {
    bool CanCreateExportable{false};
    bool CanImport{false};
    ExternalError Reason{};
};

struct ExternalAllocationInfo {
    uint64_t Size{0};
    uint64_t Offset{0};
    bool Dedicated{true};
};

struct ExternalBufferInfo {
    ExternalMemoryHandleType HandleType{ExternalMemoryHandleType::D3D12Resource};
    ExternalDeviceIdentity Device;
    RenderBackend SourceBackend{RenderBackend::D3D12};
    BufferDescriptor Descriptor;
    uint64_t NativeSize{0};
    uint32_t NativeFlags{0};
    uint32_t NativeUsage{0};
    ExternalAllocationInfo Allocation;
    // Required for OpaqueWin32; absent for D3D12Resource. Preserve the exported allocation parameters.
    std::optional<ExternalVulkanMemoryInfo> VulkanMemory;
};

struct ExternalTextureInfo {
    ExternalMemoryHandleType HandleType{ExternalMemoryHandleType::D3D12Resource};
    ExternalDeviceIdentity Device;
    RenderBackend SourceBackend{RenderBackend::D3D12};
    TextureDescriptor Descriptor;
    uint32_t NativeFormat{0};
    uint32_t NativeFlags{0};
    uint32_t NativeUsage{0};
    uint32_t NativeTiling{0};
    ExternalAllocationInfo Allocation;
    // Required for OpaqueWin32; absent for D3D12Resource. Preserve the exported allocation parameters.
    std::optional<ExternalVulkanMemoryInfo> VulkanMemory;
    bool AllowFormatReinterpretation{false};
};

struct ExternalFenceInfo {
    ExternalFenceHandleType HandleType{ExternalFenceHandleType::D3D12Fence};
    ExternalDeviceIdentity Device;
    // Required for OpaqueWin32; absent for D3D12Fence.
    std::optional<ExternalVulkanIdentity> VulkanIdentity;
};

struct ExternalBufferExport {
    ExternalBufferInfo Info;
    OwnedExternalHandle Handle;
};

struct ExternalTextureExport {
    ExternalTextureInfo Info;
    OwnedExternalHandle Handle;
};

struct ExternalFenceExport {
    ExternalFenceInfo Info;
    OwnedExternalHandle Handle;
};

/// Borrowed from Device; all resources and GPU uses must finish before Device destruction.
/// Import borrows the handle on both success and failure and never submits or waits.
/// Caller guarantees valid descriptors, matching metadata and live objects from this device.
/// Export requires a CreateExportable object. These preconditions are not validated.
class ExternalResourceExtension {
public:
    virtual ~ExternalResourceExtension() noexcept = default;
    /// Returns an identity or its unavailability reason; queries never log an error.
    virtual ExternalResult<ExternalDeviceIdentity> GetDeviceIdentity() const noexcept = 0;
    virtual ExternalSupport QueryBufferSupport(const BufferDescriptor& desc, const ExternalResourceDescriptor& external = {}) const noexcept = 0;
    virtual ExternalSupport QueryTextureSupport(const TextureDescriptor& desc, const ExternalTextureDescriptor& external = {}) const noexcept = 0;
    virtual ExternalSupport QueryFenceSupport(ExternalFenceHandleType type) const noexcept = 0;
    virtual ExternalResult<unique_ptr<Buffer>> CreateExportableBuffer(const BufferDescriptor& desc, const ExternalResourceDescriptor& external = {}) noexcept = 0;
    virtual ExternalResult<unique_ptr<Texture>> CreateExportableTexture(const TextureDescriptor& desc, const ExternalTextureDescriptor& external = {}) noexcept = 0;
    virtual ExternalResult<unique_ptr<Fence>> CreateExportableFence(const ExternalFenceDescriptor& desc = {}) noexcept = 0;
    virtual ExternalResult<ExternalBufferExport> ExportBuffer(Buffer* buffer) noexcept = 0;
    virtual ExternalResult<ExternalTextureExport> ExportTexture(Texture* texture) noexcept = 0;
    virtual ExternalResult<ExternalFenceExport> ExportFence(Fence* fence) noexcept = 0;
    virtual ExternalResult<unique_ptr<Buffer>> ImportBuffer(BorrowedExternalHandle handle, const ExternalBufferInfo& info) noexcept = 0;
    virtual ExternalResult<unique_ptr<Texture>> ImportTexture(BorrowedExternalHandle handle, const ExternalTextureInfo& info) noexcept = 0;
    virtual ExternalResult<unique_ptr<Fence>> ImportFence(BorrowedExternalHandle handle, const ExternalFenceInfo& info) noexcept = 0;
};

namespace detail {
std::string_view ExternalHandleName(ExternalMemoryHandleType type) noexcept;
std::string_view ExternalHandleName(ExternalFenceHandleType type) noexcept;
ExternalError LogExternalError(std::string_view operation, RenderBackend backend, ExternalError error, std::string_view parameters) noexcept;
}  // namespace detail
}  // namespace radray::render
