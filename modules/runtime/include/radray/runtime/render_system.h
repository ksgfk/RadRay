#pragma once

#include <filesystem>
#include <radray/render/render_pass_registry.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/graphics_pipeline_cache.h>
#include <radray/runtime_type.h>

namespace radray {
class ShaderProgramCache;

/// Device-scoped shader, graphics pipeline and render-pass services.
class RenderSystem {
public:
    explicit RenderSystem(render::Device* device, std::filesystem::path shaderSourceRoot = {}, vector<std::filesystem::path> shaderIncludePaths = {});
    RenderSystem(const RenderSystem&) = delete;
    RenderSystem(RenderSystem&&) = delete;
    RenderSystem& operator=(const RenderSystem&) = delete;
    RenderSystem& operator=(RenderSystem&&) = delete;
    ~RenderSystem() noexcept;

    [[nodiscard]] bool OnInitialize();
    /// All consumers stopped and GPU idle; accepts partial initialization.
    void OnShutdown() noexcept;
    render::RenderPassRegistry* GetRenderPassRegistry() const noexcept { return _renderPassRegistry.get(); }

    Nullable<ShaderProgram*> GetOrCreateShaderProgram(const ShaderProgramRequest& request);
    Nullable<ShaderProgram*> GetOrCreateShaderProgram(
        std::span<const byte> artifact,
        const shader::GpuArtifactHash& expectedIdentity,
        const render::ShaderProgramLayoutRecipe& recipe = {});
    GraphicsPipelineCache& GetGraphicsPipelineCache() const noexcept;
    size_t GetShaderProgramCacheSize() const noexcept;
    size_t GetShaderArtifactCacheSize() const noexcept;
    bool InvalidateShaderSource(std::string_view sourceName);

private:
    render::Device* _device;
    std::filesystem::path _shaderSourceRoot;
    vector<std::filesystem::path> _shaderIncludePaths;
    unique_ptr<render::RenderPassRegistry> _renderPassRegistry;
    unique_ptr<ShaderProgramCache> _shaderCache;
    unique_ptr<GraphicsPipelineCache> _graphicsPipelines;
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
};

}  // namespace radray
