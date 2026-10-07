#include <radray/runtime/render_system.h>

#include "shader_program_cache.h"
#include <radray/logger.h>

namespace radray {
RenderSystem::RenderSystem(render::Device* device, std::filesystem::path shaderSourceRoot, vector<std::filesystem::path> shaderIncludePaths)
    : _device(device), _shaderSourceRoot(std::move(shaderSourceRoot)), _shaderIncludePaths(std::move(shaderIncludePaths)) {
    RADRAY_ASSERT(_device != nullptr);
}
RenderSystem::~RenderSystem() noexcept { OnShutdown(); }
void RenderSystem::OnShutdown() noexcept {
    _graphicsPipelines.reset();
    _shaderCache.reset();
    _renderPassRegistry.reset();
}
bool RenderSystem::OnInitialize() {
    _graphicsPipelines = make_unique<GraphicsPipelineCache>(_device);
    _renderPassRegistry = make_unique<render::RenderPassRegistry>(_device);
    _shaderCache = make_unique<ShaderProgramCache>(*_device, _shaderSourceRoot, _shaderIncludePaths);
    return true;
}

Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(const ShaderProgramRequest& request) {
    return _shaderCache ? _shaderCache->GetOrCreateShaderProgram(request) : nullptr;
}
Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(std::span<const byte> bytes, const shader::GpuArtifactHash& identity,
                                                                const render::ShaderProgramLayoutRecipe& recipe) {
    return _shaderCache ? _shaderCache->GetOrCreateShaderProgram(bytes, identity, recipe) : nullptr;
}
GraphicsPipelineCache& RenderSystem::GetGraphicsPipelineCache() const noexcept { return *_graphicsPipelines; }
size_t RenderSystem::GetShaderProgramCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetProgramCount() : 0; }
size_t RenderSystem::GetShaderArtifactCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetArtifactCount() : 0; }
bool RenderSystem::InvalidateShaderSource(std::string_view sourceName) { return _shaderCache && _shaderCache->InvalidateSource(sourceName); }

}  // namespace radray
