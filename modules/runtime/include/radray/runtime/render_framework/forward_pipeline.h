#pragma once

#include <cstdint>

#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray {

class ForwardPipeline final : public RenderPipeline {
public:
    static constexpr std::uint32_t kObjectBindingGroup = 0;
    static constexpr std::uint32_t kPipelineBindingGroup = 1;
    // Shader convention only; this group is intentionally not provider-reserved.
    static constexpr std::uint32_t kMaterialBindingGroup = 2;

    ForwardPipeline() noexcept = default;
    ~ForwardPipeline() noexcept override;

    const PipelineBindingPolicy& GetShaderBindingPolicy() const noexcept override;
};

}  // namespace radray
