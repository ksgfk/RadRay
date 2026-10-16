#include <radray/runtime/render_system.h>

#include "shader_program_cache.h"
#include <algorithm>
#include <radray/hash.h>
#include <radray/logger.h>

namespace radray {
RenderSystem::RenderSystem(render::Device* device, std::filesystem::path shaderSourceRoot, vector<std::filesystem::path> shaderIncludePaths)
    : _device(device), _shaderSourceRoot(std::move(shaderSourceRoot)), _shaderIncludePaths(std::move(shaderIncludePaths)) {
    RADRAY_ASSERT(_device != nullptr);
}
RenderSystem::~RenderSystem() noexcept { OnShutdown(); }
void RenderSystem::OnShutdown() noexcept {
    _graphicsPipelines.reset();
    _programIds.clear();
    _adoptedPrograms.clear();
    _shaderCache.reset();
}
bool RenderSystem::OnInitialize() {
    if (_shaderCache || _graphicsPipelines) return false;
    _graphicsPipelines = make_unique<GraphicsPipelineCache>(_device);
    _shaderCache = make_unique<ShaderProgramCache>(*_device, _shaderSourceRoot, _shaderIncludePaths);
    return true;
}

Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(const ShaderProgramRequest& request) {
    return RememberProgram(_shaderCache ? _shaderCache->GetOrCreateShaderProgram(request) : nullptr);
}
Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(std::span<const byte> bytes, const shader::GpuArtifactHash& identity,
                                                                const render::ShaderProgramLayoutRecipe& recipe) {
    return RememberProgram(_shaderCache ? _shaderCache->GetOrCreateShaderProgram(bytes, identity, recipe) : nullptr);
}
Nullable<ShaderProgram*> RenderSystem::RememberProgram(Nullable<ShaderProgram*> program) {
    if (!program) return nullptr;
    std::lock_guard lock{_programMutex};
    _programIds.emplace(program->GetId().Value, program.Get());
    return program;
}
Nullable<ShaderProgram*> RenderSystem::ResolveShaderProgram(ShaderProgramId id) const {
    std::lock_guard lock{_programMutex};
    const auto found = _programIds.find(id.Value);
    return found == _programIds.end() ? nullptr : found->second;
}
Nullable<ShaderProgram*> RenderSystem::AdoptShaderProgram(unique_ptr<ShaderProgram> program) {
    if (!_shaderCache || !program || program->GetDevice() != _device) return nullptr;
    auto* result = program.get();
    std::lock_guard lock{_programMutex};
    _programIds.emplace(result->GetId().Value, result);
    _adoptedPrograms.push_back(std::move(program));
    return result;
}
GraphicsPipelineCache& RenderSystem::GetGraphicsPipelineCache() const noexcept { return *_graphicsPipelines; }
size_t RenderSystem::GetShaderProgramCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetProgramCount() : 0; }
size_t RenderSystem::GetShaderArtifactCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetArtifactCount() : 0; }
bool RenderSystem::InvalidateShaderSource(std::string_view sourceName) { return _shaderCache && _shaderCache->InvalidateSource(sourceName); }

}  // namespace radray
