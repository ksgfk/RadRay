#pragma once

namespace radray {

/// Render-side proxy for a primitive component.
/// Corresponds to UE5's FPrimitiveSceneProxy.
class PrimitiveSceneProxy {
public:
    PrimitiveSceneProxy() noexcept = default;
    PrimitiveSceneProxy(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy(PrimitiveSceneProxy&&) = delete;
    PrimitiveSceneProxy& operator=(const PrimitiveSceneProxy&) = delete;
    PrimitiveSceneProxy& operator=(PrimitiveSceneProxy&&) = delete;
    virtual ~PrimitiveSceneProxy() noexcept;
};

}  // namespace radray
