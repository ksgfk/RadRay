#include <radray/runtime/render_system.h>

#include "shader_program_cache.h"

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

RenderSystem::RenderSystem(Application* app, uint32_t flightCount) : _app(app), _frameUpdates(flightCount) {
    if (flightCount == 0) RADRAY_ABORT("Scene delivery requires at least one flight");
}

RenderSystem::~RenderSystem() noexcept {
    OnShutdown();
}

void RenderSystem::OnShutdown() noexcept {
    AbandonUnpublishedFramesGT();
    _frameUpdates.clear();
    _scene = Scene{};
    _shaderCache.reset();
    _renderPassRegistry.reset();
}

bool RenderSystem::OnInitialize() {
    auto gpu = _gpuSystem;
    if (_app == nullptr || !gpu || gpu->GetDevice() == nullptr) {
        RADRAY_ERR_LOG("initialize RenderSystem failed: {}", "Application, GpuSystem or Device is missing");
        return false;
    }
    render::Device* device = gpu->GetDevice();
    if (_frameUpdates.size() != gpu->GetFlightDataCount()) {
        RADRAY_ERR_LOG("initialize RenderSystem failed: {}", "Scene flight count does not match GpuSystem");
        return false;
    }
    _renderPassRegistry = make_unique<render::RenderPassRegistry>(device);
    _shaderCache = make_unique<ShaderProgramCache>(*device, _app->GetShaderSourceRoot(), _app->GetShaderIncludePaths());
    return true;
}

SceneUpdateBatch& RenderSystem::GetFrameUpdateBatch(uint32_t flightIndex) {
    if (flightIndex >= _frameUpdates.size()) RADRAY_ABORT("Invalid scene flight index");
    return _frameUpdates[flightIndex];
}

SceneUpdateBatch& RenderSystem::GetFrameUpdateBatchGT(uint32_t flightIndex) {
    return GetFrameUpdateBatch(flightIndex);
}

void RenderSystem::PrepareFrameGT(World& world, const AppUpdateContext& ctx) {
    (void)world;
    (void)GetFrameUpdateBatchGT(ctx.FlightIndex);
}

void RenderSystem::ConsumeRenderUpdates(uint32_t flightIndex) {
    _scene.Apply(GetFrameUpdateBatch(flightIndex));
}

void RenderSystem::OnFlightCompletedGT(const FlightCompletion& completion) {
    GetFrameUpdateBatch(completion.FlightIndex) = {};
}

void RenderSystem::AbandonUnpublishedFrameGT(uint32_t flightIndex) {
    GetFrameUpdateBatchGT(flightIndex) = {};
}

void RenderSystem::AbandonUnpublishedFramesGT() {
    for (auto& batch : _frameUpdates) batch = {};
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
