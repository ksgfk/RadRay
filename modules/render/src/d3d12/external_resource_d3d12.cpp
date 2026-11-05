#include "external_resource_d3d12.h"

#include <limits>

#include <cstring>
#include <radray/render/backend/d3d12_impl.h>

namespace radray::render::d3d12 {
namespace {
ExternalError Fail(std::string_view op, ExternalError e, std::string_view parameters = {}) noexcept {
    return radray::render::detail::LogExternalError(op, RenderBackend::D3D12, e, parameters);
}
ExternalError NativeError(HRESULT hr, std::string_view operation) noexcept { return {fmt::format("{} failed: {} ({:#x})", operation, GetErrorName(hr), uint32_t(hr)), ExternalNativeApi::D3D12, hr}; }
ExternalResult<OwnedExternalHandle> Share(DeviceD3D12* device, ID3D12DeviceChild* object) noexcept {
    HANDLE handle = nullptr;
    const HRESULT hr = device->_device->CreateSharedHandle(object, nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(hr)) return NativeError(hr, "ID3D12Device::CreateSharedHandle");
    return OwnedExternalHandle{reinterpret_cast<uintptr_t>(handle)};
}
D3D12_RESOURCE_DESC BufferDesc(const BufferDescriptor& desc, uint64_t size) noexcept {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (desc.Usage.HasFlag(BufferUse::UnorderedAccess)) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return d;
}
bool Matches(const D3D12_RESOURCE_DESC& actual, const D3D12_RESOURCE_DESC& expected, bool sampled = true) noexcept {
    return actual.Dimension == expected.Dimension && actual.Width == expected.Width && actual.Height == expected.Height &&
           actual.DepthOrArraySize == expected.DepthOrArraySize && actual.MipLevels == expected.MipLevels && actual.Format == expected.Format &&
           actual.SampleDesc.Count == expected.SampleDesc.Count && actual.SampleDesc.Quality == 0 && actual.Layout == expected.Layout &&
           (actual.Flags & expected.Flags) == expected.Flags && (!sampled || !(actual.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE));
}
bool SharedDefaultResource(ID3D12Resource* resource) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS flags{};
    return SUCCEEDED(resource->GetHeapProperties(&heap, &flags)) && heap.Type == D3D12_HEAP_TYPE_DEFAULT &&
           heap.CreationNodeMask == 1 && heap.VisibleNodeMask == 1 && (flags & D3D12_HEAP_FLAG_SHARED) && !(flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER);
}
template <class Native, class Output, class Input>
ExternalResult<Output> ExportObject(Input* input, std::string_view operation) noexcept {
    auto* object = static_cast<Native*>(input);
    string parameters;
    if constexpr (std::is_same_v<Input, Buffer>)
        parameters = fmt::format("size={} usage={}", object->GetDesc().Size, object->GetDesc().Usage);
    else if constexpr (std::is_same_v<Input, Texture>) {
        const auto d = object->GetDesc();
        parameters = fmt::format("dim={} {}x{} depthOrLayers={} mips={} samples={} format={} usage={} reinterpret={}", d.Dim, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.SampleCount, d.Format, d.Usage, object->_externalInfo->AllowFormatReinterpretation);
    } else
        parameters = fmt::format("lastLocalSignal={}", object->GetLastSignaledValue());
    parameters += fmt::format(" handleType={}", radray::render::detail::ExternalHandleName(object->_externalInfo->HandleType));
    auto handle = object->_externalHandle.Duplicate();
    if (auto* e = std::get_if<ExternalError>(&handle)) return Fail(operation, *e, parameters);
    return Output{*object->_externalInfo, std::get<OwnedExternalHandle>(std::move(handle))};
}
}  // namespace

ExternalResourceD3D12::ExternalResourceD3D12(DeviceD3D12* device) noexcept : _device(device) {}
ExternalResult<ExternalDeviceIdentity> ExternalResourceD3D12::GetDeviceIdentity() const noexcept {
    ExternalDeviceIdentity result;
    const auto nodes = _device->_device->GetNodeCount();
    if (nodes != 1) return ExternalError{fmt::format("External resources require one D3D12 node; device has {} nodes", nodes)};
    const auto luid = _device->_device->GetAdapterLuid();
    std::memcpy(result.Luid.data(), &luid, result.Luid.size());
    result.NodeMask = 1;
    return result;
}
ExternalSupport ExternalResourceD3D12::QueryBufferSupport(const BufferDescriptor& d, const ExternalResourceDescriptor& e) const noexcept {
    if (e.HandleType != ExternalMemoryHandleType::D3D12Resource) return {false, false, {"D3D12 supports only D3D12_RESOURCE memory handles"}};
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return {false, false, *error};
    if (_device->GetCapabilities().Limits.MaxBufferSize && d.Size > _device->GetCapabilities().Limits.MaxBufferSize) return {false, false, {"Buffer size exceeds the D3D12 device limit"}};
    return {true, true, {}};
}
ExternalSupport ExternalResourceD3D12::QueryTextureSupport(const TextureDescriptor& d, const ExternalTextureDescriptor& e) const noexcept {
    if (e.HandleType != ExternalMemoryHandleType::D3D12Resource) return {false, false, {"D3D12 supports only D3D12_RESOURCE memory handles"}};
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return {false, false, *error};
    if (!e.AllowFormatReinterpretation && IsDepthStencilFormat(d.Format) && d.Usage.HasFlag(TextureUse::Resource))
        return {false, false, {"D3D12 sampled depth/stencil textures require AllowFormatReinterpretation"}};
    HRESULT status = S_OK;
    const auto support = _device->QueryTextureSupportWithNativeStatus({d.Dim, d.Format, d.Usage}, status);
    if (FAILED(status)) return {false, false, NativeError(status, "ID3D12Device::CheckFeatureSupport(texture)")};
    if (!support.Supported || !support.SampleCounts.HasFlag(static_cast<SampleCount>(d.SampleCount)))
        return {false, false, {fmt::format("D3D12 does not support texture format={} usage={} samples={}", d.Format, d.Usage, d.SampleCount)}};
    if (d.Width > support.MaxWidth || d.Height > support.MaxHeight || d.MipLevels > support.MaxMipLevels ||
        d.DepthOrArraySize > (d.Dim == TextureDimension::Dim3D ? support.MaxDepth : support.MaxArrayLayers))
        return {false, false, {"Texture dimensions, mip count or layer count exceed the D3D12 image limits"}};
    const auto native = MakeTextureResourceDesc(d, e.AllowFormatReinterpretation);
    if (native.Format == DXGI_FORMAT_UNKNOWN) return {false, false, {"D3D12 has no compatible resource format"}};
    return {true, true, {}};
}
ExternalSupport ExternalResourceD3D12::QueryFenceSupport(ExternalFenceHandleType type) const noexcept {
    if (type != ExternalFenceHandleType::D3D12Fence) return {false, false, {"D3D12 supports only D3D12_FENCE synchronization handles"}};
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return {false, false, *error};
    return {true, true, {}};
}

ExternalResult<unique_ptr<Buffer>> ExternalResourceD3D12::CreateExportableBuffer(const BufferDescriptor& desc, const ExternalResourceDescriptor& external) noexcept {
    const auto params = fmt::format("handleType={} size={} usage={}", radray::render::detail::ExternalHandleName(external.HandleType), desc.Size, desc.Usage);
    const auto support = QueryBufferSupport(desc, external);
    if (!support.CanCreateExportable) return Fail("CreateExportableBuffer", support.Reason, params);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("CreateExportableBuffer", *error, params);
    uint64_t nativeSize = desc.Size;
    if (desc.Usage.HasFlag(BufferUse::CBuffer)) {
        if (nativeSize > std::numeric_limits<uint64_t>::max() - 255) return Fail("CreateExportableBuffer", {"Aligned buffer size would overflow uint64_t"}, params);
        nativeSize = (nativeSize + 255) & ~uint64_t(255);
    }
    auto d = BufferDesc(desc, nativeSize);
    UINT64 padded = 0;
    _device->_device->GetCopyableFootprints(&d, 0, 1, 0, nullptr, nullptr, nullptr, &padded);
    if (padded == std::numeric_limits<uint64_t>::max()) return Fail("CreateExportableBuffer", {"GetCopyableFootprints rejected the shared buffer descriptor"}, params);
    nativeSize = std::max(nativeSize, padded);
    d.Width = nativeSize;
    const auto allocation = _device->_device->GetResourceAllocationInfo(0, 1, &d);
    if (allocation.SizeInBytes == std::numeric_limits<uint64_t>::max()) return Fail("CreateExportableBuffer", {"GetResourceAllocationInfo rejected the shared resource descriptor"}, params);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> native;
    const auto hr = _device->_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&native));
    if (FAILED(hr)) return Fail("CreateExportableBuffer", NativeError(hr, "ID3D12Device::CreateCommittedResource"), params);
    auto handle = Share(_device, native.Get());
    if (auto* error = std::get_if<ExternalError>(&handle)) return Fail("CreateExportableBuffer", *error, params);
    auto result = make_unique<BufferD3D12>(_device, std::move(native), ComPtr<D3D12MA::Allocation>{});
    result->_reqSize = desc.Size;
    result->_memory = desc.Memory;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints | ResourceHint::Dedicated;
    result->_externalInfo = ExternalBufferInfo{external.HandleType, std::get<ExternalDeviceIdentity>(identity), RenderBackend::D3D12, result->GetDesc(), nativeSize, uint32_t(d.Flags), 0, {allocation.SizeInBytes, 0, true}, std::nullopt};
    result->_externalHandle = std::get<OwnedExternalHandle>(std::move(handle));
    return unique_ptr<Buffer>{std::move(result)};
}
ExternalResult<unique_ptr<Texture>> ExternalResourceD3D12::CreateExportableTexture(const TextureDescriptor& desc, const ExternalTextureDescriptor& external) noexcept {
    const auto params = fmt::format("handleType={} dim={} {}x{} depthOrLayers={} mips={} samples={} format={} usage={} reinterpret={}", radray::render::detail::ExternalHandleName(external.HandleType), desc.Dim, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, desc.Format, desc.Usage, external.AllowFormatReinterpretation);
    const auto support = QueryTextureSupport(desc, external);
    if (!support.CanCreateExportable) return Fail("CreateExportableTexture", support.Reason, params);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("CreateExportableTexture", *error, params);
    const auto d = MakeTextureResourceDesc(desc, external.AllowFormatReinterpretation);
    const auto allocation = _device->_device->GetResourceAllocationInfo(0, 1, &d);
    if (allocation.SizeInBytes == std::numeric_limits<uint64_t>::max()) return Fail("CreateExportableTexture", {"GetResourceAllocationInfo rejected the shared resource descriptor"}, params);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    ComPtr<ID3D12Resource> native;
    const auto hr = _device->_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&native));
    if (FAILED(hr)) return Fail("CreateExportableTexture", NativeError(hr, "ID3D12Device::CreateCommittedResource"), params);
    auto handle = Share(_device, native.Get());
    if (auto* error = std::get_if<ExternalError>(&handle)) return Fail("CreateExportableTexture", *error, params);
    auto result = make_unique<TextureD3D12>(_device, std::move(native), ComPtr<D3D12MA::Allocation>{});
    result->_dimension = desc.Dim;
    result->_format = desc.Format;
    result->_memory = desc.Memory;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints | ResourceHint::Dedicated;
    result->_externalInfo = ExternalTextureInfo{external.HandleType, std::get<ExternalDeviceIdentity>(identity), RenderBackend::D3D12, result->GetDesc(), uint32_t(d.Format), uint32_t(d.Flags), 0, uint32_t(d.Layout), {allocation.SizeInBytes, 0, true}, std::nullopt};
    result->_externalInfo->AllowFormatReinterpretation = external.AllowFormatReinterpretation;
    result->_externalHandle = std::get<OwnedExternalHandle>(std::move(handle));
    return unique_ptr<Texture>{std::move(result)};
}
ExternalResult<unique_ptr<Fence>> ExternalResourceD3D12::CreateExportableFence(const ExternalFenceDescriptor& desc) noexcept {
    const auto params = fmt::format("handleType={} initialValue={}", radray::render::detail::ExternalHandleName(desc.HandleType), desc.InitialValue);
    const auto support = QueryFenceSupport(desc.HandleType);
    if (!support.CanCreateExportable) return Fail("CreateExportableFence", support.Reason, params);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("CreateExportableFence", *error, params);
    ComPtr<ID3D12Fence> native;
    const auto hr = _device->_device->CreateFence(desc.InitialValue, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&native));
    if (FAILED(hr)) return Fail("CreateExportableFence", NativeError(hr, "ID3D12Device::CreateFence"), params);
    auto handle = Share(_device, native.Get());
    if (auto* error = std::get_if<ExternalError>(&handle)) return Fail("CreateExportableFence", *error, params);
    auto event = Win32Event::Create(false);
    if (!event) {
        const auto error = GetLastError();
        return Fail("CreateExportableFence", {"CreateEvent failed for the shared fence", ExternalNativeApi::Win32, error}, params);
    }
    auto result = make_unique<FenceD3D12>(_device, std::move(native), std::move(*event));
    result->_fenceValue = 1;
    result->_externalInfo = ExternalFenceInfo{desc.HandleType, std::get<ExternalDeviceIdentity>(identity), std::nullopt};
    result->_externalHandle = std::get<OwnedExternalHandle>(std::move(handle));
    return unique_ptr<Fence>{std::move(result)};
}
ExternalResult<ExternalBufferExport> ExternalResourceD3D12::ExportBuffer(Buffer* b) noexcept { return ExportObject<BufferD3D12, ExternalBufferExport>(b, "ExportBuffer"); }
ExternalResult<ExternalTextureExport> ExternalResourceD3D12::ExportTexture(Texture* t) noexcept { return ExportObject<TextureD3D12, ExternalTextureExport>(t, "ExportTexture"); }
ExternalResult<ExternalFenceExport> ExternalResourceD3D12::ExportFence(Fence* f) noexcept { return ExportObject<FenceD3D12, ExternalFenceExport>(f, "ExportFence"); }

ExternalResult<unique_ptr<Buffer>> ExternalResourceD3D12::ImportBuffer(BorrowedExternalHandle handle, const ExternalBufferInfo& info) noexcept {
    const auto params = fmt::format("handleType={} size={} nativeSize={} usage={}", radray::render::detail::ExternalHandleName(info.HandleType), info.Descriptor.Size, info.NativeSize, info.Descriptor.Usage);
    if (info.HandleType != ExternalMemoryHandleType::D3D12Resource) return Fail("ImportBuffer", {"D3D12 does not support the requested external handle type"}, params);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("ImportBuffer", *error, params);
    if (info.Device != std::get<ExternalDeviceIdentity>(identity)) return Fail("ImportBuffer", {"Exported device LUID or node mask does not match the importing device"}, params);
    const auto support = QueryBufferSupport(info.Descriptor, {info.HandleType});
    if (!support.CanImport) return Fail("ImportBuffer", support.Reason, params);
    ComPtr<ID3D12Resource> native;
    const auto hr = _device->_device->OpenSharedHandle(reinterpret_cast<HANDLE>(handle.Value), IID_PPV_ARGS(&native));
    if (FAILED(hr)) return Fail("ImportBuffer", NativeError(hr, "ID3D12Device::OpenSharedHandle"), params);
    const auto d = native->GetDesc();
    if (!Matches(d, BufferDesc(info.Descriptor, info.NativeSize)) || !SharedDefaultResource(native.Get())) return Fail("ImportBuffer", {"Exported buffer metadata or native resource parameters do not match"}, params);
    if (info.SourceBackend == RenderBackend::D3D12 && (info.NativeFlags != uint32_t(d.Flags) || info.Allocation.Size != _device->_device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes))
        return Fail("ImportBuffer", {"Exported buffer metadata or native resource parameters do not match"}, params);
    auto result = make_unique<BufferD3D12>(_device, std::move(native), ComPtr<D3D12MA::Allocation>{});
    result->_reqSize = info.Descriptor.Size;
    result->_memory = MemoryType::Device;
    result->_usage = info.Descriptor.Usage;
    result->_hints = info.Descriptor.Hints | ResourceHint::Dedicated;
    result->_externalInfo = info;
    return unique_ptr<Buffer>{std::move(result)};
}
ExternalResult<unique_ptr<Texture>> ExternalResourceD3D12::ImportTexture(BorrowedExternalHandle handle, const ExternalTextureInfo& info) noexcept {
    const auto& desc = info.Descriptor;
    const auto params = fmt::format("handleType={} dim={} {}x{} depthOrLayers={} mips={} samples={} format={} usage={} reinterpret={}", radray::render::detail::ExternalHandleName(info.HandleType), desc.Dim, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, desc.Format, desc.Usage, info.AllowFormatReinterpretation);
    if (info.HandleType != ExternalMemoryHandleType::D3D12Resource) return Fail("ImportTexture", {"D3D12 does not support the requested external handle type"}, params);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("ImportTexture", *error, params);
    if (info.Device != std::get<ExternalDeviceIdentity>(identity)) return Fail("ImportTexture", {"Exported device LUID or node mask does not match the importing device"}, params);
    const auto support = QueryTextureSupport(desc, {info.HandleType, info.AllowFormatReinterpretation});
    if (!support.CanImport) return Fail("ImportTexture", support.Reason, params);
    ComPtr<ID3D12Resource> native;
    const auto hr = _device->_device->OpenSharedHandle(reinterpret_cast<HANDLE>(handle.Value), IID_PPV_ARGS(&native));
    if (FAILED(hr)) return Fail("ImportTexture", NativeError(hr, "ID3D12Device::OpenSharedHandle"), params);
    const auto d = native->GetDesc();
    if (!Matches(d, MakeTextureResourceDesc(desc, info.AllowFormatReinterpretation), desc.Usage.HasFlag(TextureUse::Resource)) || !SharedDefaultResource(native.Get())) return Fail("ImportTexture", {"Exported texture metadata or native resource parameters do not match"}, params);
    if (info.SourceBackend == RenderBackend::D3D12 && (info.NativeFormat != uint32_t(d.Format) || info.NativeFlags != uint32_t(d.Flags) || info.NativeTiling != uint32_t(d.Layout) || info.Allocation.Size != _device->_device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes))
        return Fail("ImportTexture", {"Exported texture metadata or native resource parameters do not match"}, params);
    auto result = make_unique<TextureD3D12>(_device, std::move(native), ComPtr<D3D12MA::Allocation>{});
    result->_dimension = desc.Dim;
    result->_format = desc.Format;
    result->_memory = MemoryType::Device;
    result->_usage = desc.Usage;
    result->_hints = desc.Hints | ResourceHint::Dedicated;
    result->_externalInfo = info;
    return unique_ptr<Texture>{std::move(result)};
}
ExternalResult<unique_ptr<Fence>> ExternalResourceD3D12::ImportFence(BorrowedExternalHandle handle, const ExternalFenceInfo& info) noexcept {
    const auto params = fmt::format("handleType={} nodeMask={} handle={:#x}", radray::render::detail::ExternalHandleName(info.HandleType), info.Device.NodeMask, handle.Value);
    if (info.HandleType != ExternalFenceHandleType::D3D12Fence) return Fail("ImportFence", {"D3D12 does not support the requested external handle type"}, params);
    const auto identity = GetDeviceIdentity();
    if (const auto* error = std::get_if<ExternalError>(&identity)) return Fail("ImportFence", *error, params);
    if (info.Device != std::get<ExternalDeviceIdentity>(identity)) return Fail("ImportFence", {"Exported device LUID or node mask does not match the importing device"}, params);
    ComPtr<ID3D12Fence> native;
    const auto hr = _device->_device->OpenSharedHandle(reinterpret_cast<HANDLE>(handle.Value), IID_PPV_ARGS(&native));
    if (FAILED(hr)) return Fail("ImportFence", NativeError(hr, "ID3D12Device::OpenSharedHandle"), params);
    auto event = Win32Event::Create(false);
    if (!event) {
        const auto error = GetLastError();
        return Fail("ImportFence", {"CreateEvent failed for the shared fence", ExternalNativeApi::Win32, error}, params);
    }
    auto result = make_unique<FenceD3D12>(_device, std::move(native), std::move(*event));
    result->_fenceValue = 1;
    result->_externalInfo = info;
    return unique_ptr<Fence>{std::move(result)};
}
}  // namespace radray::render::d3d12
