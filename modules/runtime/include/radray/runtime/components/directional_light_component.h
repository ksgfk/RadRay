#pragma once

#include <radray/runtime/components/light_component.h>

namespace radray {

/// 方向光组件 (对应 UE5 的 UDirectionalLightComponent 精简版)。
///
/// 光照方向取自组件世界旋转的 +Z (与 LightComponent::GetLightDirection 一致), 无位置概念。
/// 携带级联阴影 (CSM) 配置: 级联数量、阴影距离、划分混合系数、阴影图分辨率、深度/法线偏移、
/// PCF 模式。ForwardPipeline 据此在阴影 pass 里逐级联计算正交光锥并渲染深度。
class DirectionalLightComponent : public LightComponent {
public:
    DirectionalLightComponent() noexcept = default;
    ~DirectionalLightComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    LightType GetLightType() const noexcept override { return LightType::Directional; }
    unique_ptr<LightSceneProxy> CreateSceneProxy() const override;

    /// 级联数量 (1..RADRAY_MAX_CASCADES=4)。
    void SetCascadeCount(uint32_t count) noexcept;
    uint32_t GetCascadeCount() const noexcept { return _cascadeCount; }

    /// 阴影覆盖距离 (从相机近平面起, 世界单位)。超出该距离不接收 CSM 阴影。
    void SetShadowDistance(float distance) noexcept;
    float GetShadowDistance() const noexcept { return _shadowDistance; }

    /// 级联划分的 对数/均匀 混合系数 [0,1]: 0 = 纯均匀分布, 1 = 纯对数分布。
    void SetCascadeSplitLambda(float lambda) noexcept;
    float GetCascadeSplitLambda() const noexcept { return _cascadeSplitLambda; }

    /// 单张级联阴影图边长 (像素)。
    void SetShadowMapResolution(uint32_t resolution) noexcept;
    uint32_t GetShadowMapResolution() const noexcept { return _shadowMapResolution; }

    void SetShadowDepthBias(float bias) noexcept;
    float GetShadowDepthBias() const noexcept { return _shadowDepthBias; }

    void SetShadowNormalBias(float bias) noexcept;
    float GetShadowNormalBias() const noexcept { return _shadowNormalBias; }

    /// PCF 模式: 0 = 单 tap, 1 = 4-tap, 2 = 5x5 tent。
    void SetShadowSoftMode(uint32_t mode) noexcept;
    uint32_t GetShadowSoftMode() const noexcept { return _shadowSoftMode; }

private:
    uint32_t _cascadeCount{4};
    float _shadowDistance{100.0f};
    float _cascadeSplitLambda{0.75f};
    uint32_t _shadowMapResolution{2048};
    float _shadowDepthBias{0.002f};
    float _shadowNormalBias{1.0f};
    uint32_t _shadowSoftMode{2};
};

template <>
struct RuntimeTypeTrait<DirectionalLightComponent> {
    static constexpr RuntimeTypeId value{0x6f2b1c84, 0x9a3d, 0x4e17, 0xb2, 0x6e, 0x51, 0xa8, 0x7c, 0x0d, 0x33, 0x91};
    using Bases = std::tuple<LightComponent>;
};

}  // namespace radray
