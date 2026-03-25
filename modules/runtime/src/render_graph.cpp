#include <radray/runtime/render_graph.h>

namespace radray {

uint32_t RenderGraph::GetNextResHandleId() const noexcept {
    return static_cast<uint32_t>(_resources.size()) + 1;
}

uint32_t RenderGraph::GetNextPassHandleId() const noexcept {
    return static_cast<uint32_t>(_passes.size()) + 1;
}

RGTextureHandle RenderGraph::ImportTexture(std::string_view name, const RGImportedTextureDesc& desc) {
    uint32_t id = this->GetNextResHandleId();
    RGResource resource{};
    resource.Name = name;
    resource.Id = id;
    resource.Origin = RGResourceOrigin::Imported;
    RGResource::TextureRecord textureRecord{};
    textureRecord.Desc = desc.NativeDesc;
    textureRecord.ImportedHandle = desc.Texture;
    textureRecord.ImportedDefaultView = desc.DefaultView;
    textureRecord.ImportedInitialState = desc.InitialState;
    textureRecord.ImportedInitialView = desc.InitialView;
    resource.Data = std::move(textureRecord);
    _resources.emplace_back(std::move(resource));
    return RGTextureHandle{id};
}

RGBufferHandle RenderGraph::ImportBuffer(std::string_view name, const RGImportedBufferDesc& desc) {
    uint32_t id = this->GetNextResHandleId();
    RGResource resource{};
    resource.Name = name;
    resource.Id = id;
    resource.Origin = RGResourceOrigin::Imported;
    RGResource::BufferRecord bufferRecord{};
    bufferRecord.Desc = desc.NativeDesc;
    bufferRecord.ImportedHandle = desc.Buffer;
    bufferRecord.ImportedInitialState = desc.InitialState;
    bufferRecord.ImportedInitialView = desc.InitialView;
    resource.Data = std::move(bufferRecord);
    _resources.emplace_back(std::move(resource));
    return RGBufferHandle{id};
}

RGTextureHandle RenderGraph::CreateTexture(std::string_view name, const RGTextureDesc& desc) {
    uint32_t id = this->GetNextResHandleId();
    RGResource resource{};
    resource.Name = name;
    resource.Id = id;
    resource.Origin = RGResourceOrigin::Graph;
    resource.ForceNonTransient = desc.ForceNonTransient;
    RGResource::TextureRecord textureRecord{};
    textureRecord.Desc = desc.NativeDesc;
    resource.Data = std::move(textureRecord);
    _resources.emplace_back(std::move(resource));
    return RGTextureHandle{id};
}

RGBufferHandle RenderGraph::CreateBuffer(std::string_view name, const RGBufferDesc& desc) {
    uint32_t id = this->GetNextResHandleId();
    RGResource resource{};
    resource.Name = name;
    resource.Id = id;
    resource.Origin = RGResourceOrigin::Graph;
    resource.ForceNonTransient = desc.ForceNonTransient;
    RGResource::BufferRecord bufferRecord{};
    bufferRecord.Desc = desc.NativeDesc;
    resource.Data = std::move(bufferRecord);
    _resources.emplace_back(std::move(resource));
    return RGBufferHandle{id};
}

void RenderGraph::SetTextureExport(RGTextureHandle handle, const RGTextureBindingView& view, const RGTextureUseDesc& finalState) {
    RADRAY_ASSERT(handle.IsValid());
    uint32_t index = handle.Id - 1;
    RADRAY_ASSERT(index < _resources.size());
    RGResource& resource = _resources[index];
    RADRAY_ASSERT(std::holds_alternative<RGResource::TextureRecord>(resource.Data));
    auto& record = std::get<RGResource::TextureRecord>(resource.Data);
    resource.IsExtracted = true;
    record.ExtractedView = view;
    record.ExtractedFinalUse = finalState;
}

void RenderGraph::SetBufferExport(RGBufferHandle handle, const RGBufferBindingView& view, const RGBufferUseDesc& finalState) {
    RADRAY_ASSERT(handle.IsValid());
    uint32_t index = handle.Id - 1;
    RADRAY_ASSERT(index < _resources.size());
    RGResource& resource = _resources[index];
    RADRAY_ASSERT(std::holds_alternative<RGResource::BufferRecord>(resource.Data));
    auto& record = std::get<RGResource::BufferRecord>(resource.Data);
    resource.IsExtracted = true;
    record.ExtractedView = view;
    record.ExtractedFinalUse = finalState;
}

GpuTextureHandle RenderGraph::GetExtractedTexture(RGTextureHandle handle) const {
    RADRAY_ASSERT(handle.IsValid());
    uint32_t index = handle.Id - 1;
    RADRAY_ASSERT(index < _resources.size());
    const RGResource& resource = _resources[index];
    RADRAY_ASSERT(std::holds_alternative<RGResource::TextureRecord>(resource.Data));
    RADRAY_ASSERT(resource.IsExtracted);
    const auto& record = std::get<RGResource::TextureRecord>(resource.Data);
    return record.ExtractedHandle;
}

GpuBufferHandle RenderGraph::GetExtractedBuffer(RGBufferHandle handle) const {
    RADRAY_ASSERT(handle.IsValid());
    uint32_t index = handle.Id - 1;
    RADRAY_ASSERT(index < _resources.size());
    const RGResource& resource = _resources[index];
    RADRAY_ASSERT(std::holds_alternative<RGResource::BufferRecord>(resource.Data));
    RADRAY_ASSERT(resource.IsExtracted);
    const auto& record = std::get<RGResource::BufferRecord>(resource.Data);
    return record.ExtractedHandle;
}

}  // namespace radray
