#pragma once

#include <cstdint>
#include <optional>

#include <radray/gpu/types.h>
#include <radray/nullable.h>
#include <radray/runtime/asset_handle.h>
#include <radray/runtime/gpu_pipeline_layout.h>
#include <radray/runtime/vertex_input.h>
#include <radray/types.h>

namespace radray {

class FrameSlot;
class PipelineLayoutRegistry;
class ShaderAsset;

struct GraphicsPsoKey {
    AssetHandle<ShaderAsset> Shader;
    uint32_t Variant{0};
    shared_ptr<GpuPipelineLayout> ExplicitLayout;
    vector<gpu::VertexBufferLayout> VertexLayouts;
    vector<VertexInputAttribute> VertexAttributes;
    gpu::PrimitiveState Primitive{gpu::PrimitiveState::Default()};
    std::optional<gpu::DepthStencilState> DepthStencil;
    gpu::MultiSampleState MultiSample{gpu::MultiSampleState::Default()};
    vector<gpu::ColorTargetState> ColorTargets;
    bool EnablePixelStage{true};
    friend bool operator==(const GraphicsPsoKey&, const GraphicsPsoKey&) = default;
};

struct ComputePsoKey {
    AssetHandle<ShaderAsset> Shader;
    uint32_t Variant{0};
    shared_ptr<GpuPipelineLayout> ExplicitLayout;
    friend bool operator==(const ComputePsoKey&, const ComputePsoKey&) = default;
};

class PsoCache final {
public:
    // All four collaborators are borrowed for the lifetime of this cache.
    PsoCache(gpu::Device& device, const AssetManager& assets, PipelineLayoutRegistry& layouts, Nullable<gpu::PipelineCache*> nativeCache = nullptr);
    ~PsoCache() noexcept;
    PsoCache(const PsoCache&) = delete;
    PsoCache& operator=(const PsoCache&) = delete;
    Nullable<gpu::GraphicsPipelineState*> AcquireGraphics(const GraphicsPsoKey& key);
    Nullable<gpu::ComputePipelineState*> AcquireCompute(const ComputePsoKey& key);
    // The slot must complete after the last GPU use of each invalidated version.
    void RetireInvalidated(FrameSlot& slot);

private:
    class Impl;
    unique_ptr<Impl> _impl;
};

}  // namespace radray
