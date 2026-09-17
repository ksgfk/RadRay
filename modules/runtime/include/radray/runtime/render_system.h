#pragma once

#include <span>
#include <string_view>

#include <radray/nullable.h>
#include <radray/render/render_pass_registry.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime_type.h>
#include <radray/types.h>

namespace radray {

class Application;
class GpuSystem;
class ShaderProgramCache;

/// Runtime shader/program and RHI render-pass caches. Device and flight ownership stay in GpuSystem.
/// Contract: docs/architecture/render-framework.md
class RenderSystem {
public:
    explicit RenderSystem(Application* app) noexcept;
    RenderSystem(const RenderSystem&) = delete;
    RenderSystem(RenderSystem&&) = delete;
    RenderSystem& operator=(const RenderSystem&) = delete;
    RenderSystem& operator=(RenderSystem&&) = delete;
    ~RenderSystem() noexcept;

    [[nodiscard]] bool OnInitialize(string& error);
    /// Requires GPU idle; also accepts partial initialization.
    void OnShutdown() noexcept;
    void SetGpuSystem(Nullable<GpuSystem*> gpu) noexcept { _gpuSystem = gpu; }
    render::RenderPassRegistry* GetRenderPassRegistry() const noexcept { return _renderPassRegistry.get(); }

    Nullable<ShaderProgram*> GetOrCreateShaderProgram(const ShaderProgramRequest& request);
    Nullable<ShaderProgram*> GetOrCreateShaderProgram(std::span<const byte> artifact,
                                                      const shader::GpuArtifactHash& expectedIdentity,
                                                      const render::ShaderProgramLayoutRecipe& recipe = {});
    size_t GetShaderProgramCacheSize() const noexcept;
    size_t GetShaderArtifactCacheSize() const noexcept;
    bool InvalidateShaderSource(std::string_view sourceName);

private:
    Application* _app;
    Nullable<GpuSystem*> _gpuSystem{nullptr};
    unique_ptr<render::RenderPassRegistry> _renderPassRegistry;
    unique_ptr<ShaderProgramCache> _shaderCache;
};

template <>
struct RuntimeTypeTrait<RenderSystem> {
    static constexpr RuntimeTypeId value{0x241d4e78, 0x8f4e, 0x4d1c, 0xa8, 0xb9, 0x55, 0x09, 0x61, 0x6a, 0x90, 0x24};
};

}  // namespace radray
