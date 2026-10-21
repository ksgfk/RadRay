#pragma once

#include <radray/runtime/render_framework/renderer_resources.h>
#include <radray/runtime/render_framework/renderer_frame.h>
#include <radray/runtime/render_framework/renderer_scene.h>

namespace radray {
class AppFrameContext;
class GpuSystem;
class RenderSystem;
class Renderer;

struct FrameBindingId {
    RendererFrameId Frame;
    uint32_t Index{UINT32_MAX};
    friend bool operator==(const FrameBindingId&, const FrameBindingId&) = default;
};

/// Non-owning construction capability. Its admitted storage belongs to Renderer until the host ends the GPU epoch.
class FrameBuilder {
public:
    FrameBuilder(const FrameBuilder&) = delete;
    FrameBuilder& operator=(const FrameBuilder&) = delete;
    FrameBuilder(FrameBuilder&& other) noexcept;
    FrameBuilder& operator=(FrameBuilder&& other) noexcept;
    RendererFrameId Id() const noexcept { return _id; }
    bool IsFor(const AppFrameContext& frame) const noexcept;
    bool IsFor(const Renderer& renderer, uint32_t flight, uint64_t serial) const noexcept;
    bool IsCompiled() const noexcept;
    std::optional<FramePreparationId> AddPreparation(unique_ptr<FramePreparation> preparation);
    Nullable<FramePreparation*> GetPreparation(FramePreparationId id) const noexcept;
    std::optional<ResourceId> StoreData(std::span<const byte> bytes, uint32_t stride = 0);
    bool UseResource(ResourceId id);
    bool UseMaterial(MaterialId id);
    std::optional<FrameViewId> AddView(RendererViewDescriptor descriptor);
    std::optional<FramePassId> AddPass(RendererPassDescriptor descriptor);
    std::optional<FrameDrawId> AddDraw(FramePassId pass, MaterialId material, RendererDrawDescriptor descriptor, RendererDrawValues values = {});
    bool Record(render::GraphicsCommandEncoder* encoder, FrameDrawId draw) const;
    bool Record(render::GraphicsCommandEncoder* encoder, std::span<const FrameDrawId> draws) const;
    bool RecordPass(render::GraphicsCommandEncoder* encoder, FramePassId pass) const;
    /// Native service integration: layout must remain alive until Renderer shutdown, including idle cached groups.
    /// Resource dependencies must include every referenced imported or arena resource.
    std::optional<FrameBindingId> AddBindings(render::PipelineLayout* layout, uint32_t group,
                                              std::span<const ParameterWrite> writes, std::span<const ResourceId> dependencies);
    bool Compile();
    /// Recording performs validation and binding only; it never prepares values or creates storage.
    bool Bind(render::GraphicsCommandEncoder* encoder, FrameBindingId id,
              std::span<const render::ShaderParameterDynamicOffset> offsets = {}) const;

private:
    friend class Renderer;
    FrameBuilder(Renderer* renderer, RendererFrameId id) noexcept;
    Nullable<Renderer*> _renderer;
    RendererFrameId _id;
};

/// Single safe-point owner of framework GPU representations. The borrowed host and services outlive it.
class Renderer {
public:
    Renderer(GpuSystem& host, RenderSystem& services);
    ~Renderer() noexcept;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    std::optional<FrameBuilder> BeginFrame(AppFrameContext& frame);
    std::optional<TemplateId> RegisterTemplate(const MaterialTemplateDescriptor& descriptor);
    bool UnregisterTemplate(TemplateId id);
    Nullable<const CompiledMaterialTemplate*> GetTemplate(TemplateId id) const noexcept;
    std::optional<MaterialId> RegisterMaterial(const RendererMaterialDescriptor& descriptor);
    std::optional<MaterialId> ReplaceMaterial(MaterialId old, const RendererMaterialDescriptor& descriptor);
    bool UnregisterMaterial(MaterialId id);
    bool IsMaterialValid(MaterialId id) const noexcept;
    std::span<const TemplateId> GetMaterialTemplates(MaterialId id) const noexcept;
    bool IsMaterialRetired(MaterialId id) const noexcept;
    bool IsResourceRetired(ResourceId id) const noexcept;
    RendererScenes& GetScenes() noexcept;
    RenderSystem& GetServices() const noexcept;
    render::Device* GetDevice() const noexcept;
    std::optional<ResourceId> ImportBuffer(render::Buffer* buffer);
    std::optional<ResourceId> CreateBuffer(const render::BufferDescriptor& descriptor);
    Nullable<render::Buffer*> GetBuffer(ResourceId id) const noexcept;
    std::optional<ResourceId> ImportTexture(render::Texture* texture);
    std::optional<ResourceId> CreateTextureView(ResourceId texture, render::TextureViewDescriptor descriptor);
    std::optional<ResourceId> CreateBufferRange(ResourceId buffer, render::BufferRange range, uint32_t stride = 0);
    bool RequestResourceExit(ResourceId id);
    vector<ResourceId> TakeResourceExitConfirmations();
    std::optional<render::ShaderParameterValue> GetResourceValue(ResourceId id) const;
    bool IsResourceValid(ResourceId id) const noexcept;
    /// Consume ordered host completion messages on the registry owner's safe point.
    void ProcessCompletions();
    void TrimIdleBindings();
    void SetBindingBudget(BindingCacheBudget budget);
    /// Host has stopped producing, released completed payloads, and ended all accepted command epochs.
    bool Shutdown();
    const string& GetLastError() const noexcept;

private:
    friend class FrameBuilder;
    friend class RendererScenes;
    friend class SceneGpuData;
    bool RetireResource(ResourceId id);
    struct State;
    struct CompletionLease;
    void NotifyCompletion(RendererFrameId id);
    bool UseMaterial(RendererFrameId frame, MaterialId material);
    bool UseResource(RendererFrameId frame, ResourceId resource);
    std::optional<ResourceId> StoreData(RendererFrameId frame, std::span<const byte> bytes, uint32_t stride);
    std::optional<FrameBindingId> AddBindings(RendererFrameId frame, render::PipelineLayout* layout, uint32_t group,
                                              std::span<const ParameterWrite> writes, std::span<const ResourceId> dependencies);
    bool Compile(RendererFrameId frame);
    std::optional<FrameViewId> AddView(RendererFrameId frame, RendererViewDescriptor descriptor);
    std::optional<FramePassId> AddPass(RendererFrameId frame, RendererPassDescriptor descriptor);
    std::optional<FrameDrawId> AddDraw(FramePassId pass, MaterialId material, RendererDrawDescriptor descriptor, RendererDrawValues values);
    bool CompileDraws(RendererFrameId frame);
    bool Record(render::GraphicsCommandEncoder* encoder, FrameDrawId draw) const;
    bool Record(render::GraphicsCommandEncoder* encoder, std::span<const FrameDrawId> draws) const;
    bool RecordPass(render::GraphicsCommandEncoder* encoder, FramePassId pass) const;
    bool Bind(render::GraphicsCommandEncoder* encoder, FrameBindingId id, std::span<const render::ShaderParameterDynamicOffset> offsets) const;
    void Collect();
    unique_ptr<State> _state;
};
}  // namespace radray
