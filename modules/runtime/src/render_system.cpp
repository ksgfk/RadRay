#include <radray/runtime/render_system.h>

#include "shader_program_cache.h"

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

RenderSystem::RenderSystem(Application* app, uint32_t flightCount) : _app(app), _frameUpdates(flightCount), _frameAssetRefs(flightCount) {
    if (flightCount == 0) RADRAY_ABORT("Scene delivery requires at least one flight");
}

RenderSystem::~RenderSystem() noexcept {
    OnShutdown();
}

void RenderSystem::OnShutdown() noexcept {
    AbandonUnpublishedFramesGT();
    _frameUpdates.clear();
    _frameAssetRefs.clear();
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
    world.FlushRenderUpdates(GetFrameUpdateBatchGT(ctx.FlightIndex));
    world.RetainRenderAssets(_assetManager, _frameAssetRefs[ctx.FlightIndex]);
}

void RenderSystem::ConsumeRenderUpdates(uint32_t flightIndex) {
    _scene.Apply(GetFrameUpdateBatch(flightIndex));
}

void RenderSystem::OnFlightCompletedGT(const FlightCompletion& completion) {
    GetFrameUpdateBatch(completion.FlightIndex).Clear();
    _frameAssetRefs[completion.FlightIndex].clear();
}

void RenderSystem::AbandonUnpublishedFrameGT(uint32_t flightIndex) {
    GetFrameUpdateBatchGT(flightIndex).Clear();
    _frameAssetRefs[flightIndex].clear();
}

void RenderSystem::AbandonUnpublishedFramesGT() {
    for (auto& batch : _frameUpdates) batch.Clear();
    for (auto& refs : _frameAssetRefs) refs.clear();
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
