#include <radray/runtime/render_system.h>

#include "shader_program_cache.h"

#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

RenderSystem::RenderSystem(Application* app) noexcept : _app(app) {}

RenderSystem::~RenderSystem() noexcept {
    OnShutdown();
}

void RenderSystem::OnShutdown() noexcept {
    _shaderCache.reset();
    _renderPassRegistry.reset();
}

bool RenderSystem::OnInitialize(string& error) {
    error.clear();
    auto gpu = _gpuSystem;
    if (_app == nullptr || !gpu || gpu->GetDevice() == nullptr) {
        error = "Application, GpuSystem or Device is missing";
        return false;
    }
    render::Device* device = gpu->GetDevice();
    _renderPassRegistry = make_unique<render::RenderPassRegistry>(device);
    _shaderCache = make_unique<ShaderProgramCache>(*device, _app->GetShaderSourceRoot(), _app->GetShaderIncludePaths());
    return true;
}

Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(const ShaderProgramRequest& request) {
    return _shaderCache ? _shaderCache->GetOrCreateShaderProgram(request) : nullptr;
}
Nullable<ShaderProgram*> RenderSystem::GetOrCreateShaderProgram(std::span<const byte> bytes, const shader::GpuArtifactHash& identity,
                                                                const render::ShaderProgramLayoutRecipe& recipe) {
    return _shaderCache ? _shaderCache->GetOrCreateShaderProgram(bytes, identity, recipe) : nullptr;
}
size_t RenderSystem::GetShaderProgramCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetProgramCount() : 0; }
size_t RenderSystem::GetShaderArtifactCacheSize() const noexcept { return _shaderCache ? _shaderCache->GetArtifactCount() : 0; }
bool RenderSystem::InvalidateShaderSource(std::string_view sourceName) { return _shaderCache && _shaderCache->InvalidateSource(sourceName); }

}  // namespace radray
