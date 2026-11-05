#pragma once

#include <radray/render/external_resource.h>

namespace radray::render::vulkan {
class DeviceVulkan;
class ExternalResourceVulkan final : public ExternalResourceExtension {
public:
    explicit ExternalResourceVulkan(DeviceVulkan* device) noexcept;
    ExternalResult<ExternalDeviceIdentity> GetDeviceIdentity() const noexcept override;
    ExternalSupport QueryBufferSupport(const BufferDescriptor& desc, const ExternalResourceDescriptor& external) const noexcept override;
    ExternalSupport QueryTextureSupport(const TextureDescriptor& desc, const ExternalTextureDescriptor& external) const noexcept override;
    ExternalSupport QueryFenceSupport(ExternalFenceHandleType type) const noexcept override;
    ExternalResult<unique_ptr<Buffer>> CreateExportableBuffer(const BufferDescriptor& desc, const ExternalResourceDescriptor& external) noexcept override;
    ExternalResult<unique_ptr<Texture>> CreateExportableTexture(const TextureDescriptor& desc, const ExternalTextureDescriptor& external) noexcept override;
    ExternalResult<unique_ptr<Fence>> CreateExportableFence(const ExternalFenceDescriptor& desc) noexcept override;
    ExternalResult<ExternalBufferExport> ExportBuffer(Buffer* buffer) noexcept override;
    ExternalResult<ExternalTextureExport> ExportTexture(Texture* texture) noexcept override;
    ExternalResult<ExternalFenceExport> ExportFence(Fence* fence) noexcept override;
    ExternalResult<unique_ptr<Buffer>> ImportBuffer(BorrowedExternalHandle handle, const ExternalBufferInfo& info) noexcept override;
    ExternalResult<unique_ptr<Texture>> ImportTexture(BorrowedExternalHandle handle, const ExternalTextureInfo& info) noexcept override;
    ExternalResult<unique_ptr<Fence>> ImportFence(BorrowedExternalHandle handle, const ExternalFenceInfo& info) noexcept override;

private:
    DeviceVulkan* _device;
};
}  // namespace radray::render::vulkan
