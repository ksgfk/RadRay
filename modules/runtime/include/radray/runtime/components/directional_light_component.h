#pragma once

#include <array>

#include <radray/runtime/components/light_component.h>

namespace radray {

/// 级联划分模式 (对齐 Unity URP 的级联设置)。
enum class CascadeSplitMode : uint32_t {
    /// 自动: 用 practical split scheme (对数/均匀混合, 见 CascadeSplitLambda) 计算划分。
    Automatic = 0,
    /// 手动: 用用户输入的归一化划分比例 (CascadeSplitRatios) 计算划分。
    Manual = 1,
};

/// 方向光组件 (对应 UE5 的 UDirectionalLightComponent 精简版)。
///
/// 光照方向取自组件世界旋转的 +Z (与 LightComponent::GetLightDirection 一致), 无位置概念。
/// 携带级联阴影 (CSM) 配置: 级联数量、阴影距离、划分模式 (自动/手动)、划分混合系数或
/// 手动划分比例、阴影图分辨率、深度/法线偏移、PCF 模式。
/// ForwardPipeline 据此在阴影 pass 里逐级联计算正交光锥并渲染深度。
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

    /// 级联划分模式: 自动 (混合系数) 或 手动 (划分比例)。
    void SetCascadeSplitMode(CascadeSplitMode mode) noexcept;
    CascadeSplitMode GetCascadeSplitMode() const noexcept { return _cascadeSplitMode; }

    /// 级联划分的 对数/均匀 混合系数 [0,1]: 0 = 纯均匀分布, 1 = 纯对数分布 (仅自动模式生效)。
    void SetCascadeSplitLambda(float lambda) noexcept;
    float GetCascadeSplitLambda() const noexcept { return _cascadeSplitLambda; }

    /// 手动级联划分比例 (仅手动模式生效), 对齐 Unity URP 的级联划分设置。
    /// 数组元素为 [0,1] 的累积归一化边界 (相对阴影距离): ratios[i] 是第 i 与第 i+1 级联之间的边界。
    /// 对 N 个级联只使用前 N-1 个元素, 且应满足单调递增。例如 4 级联默认 {0.067, 0.2, 0.467}:
    ///   级联0 = [0, 0.067], 级联1 = [0.067, 0.2], 级联2 = [0.2, 0.467], 级联3 = [0.467, 1]。
    void SetCascadeSplitRatios(const std::array<float, 3>& ratios) noexcept;
    const std::array<float, 3>& GetCascadeSplitRatios() const noexcept { return _cascadeSplitRatios; }

    /// 单张级联阴影图边长 (像素)。
    void SetShadowMapResolution(uint32_t resolution) noexcept;
    uint32_t GetShadowMapResolution() const noexcept { return _shadowMapResolution; }

    /// 阴影深度 bias 倍率 (无量纲, URP 风格): 渲染时逐级联乘以该级联的 texel 世界尺寸得到实际偏移,
    /// 故不同分辨率 / 不同级联 frustum 大小下表现自动一致。典型范围 0..几。
    void SetShadowDepthBias(float bias) noexcept;
    float GetShadowDepthBias() const noexcept { return _shadowDepthBias; }

    /// 阴影法线 bias 倍率 (无量纲, URP 风格): 沿法线偏移采样点, 逐级联乘以 texel 世界尺寸。
    void SetShadowNormalBias(float bias) noexcept;
    float GetShadowNormalBias() const noexcept { return _shadowNormalBias; }

    /// PCF 模式: 0 = 单 tap, 1 = 4-tap, 2 = 5x5 tent。
    void SetShadowSoftMode(uint32_t mode) noexcept;
    uint32_t GetShadowSoftMode() const noexcept { return _shadowSoftMode; }

private:
    uint32_t _cascadeCount{4};
    float _shadowDistance{100.0f};
    CascadeSplitMode _cascadeSplitMode{CascadeSplitMode::Automatic};
    float _cascadeSplitLambda{0.75f};
    std::array<float, 3> _cascadeSplitRatios{0.067f, 0.2f, 0.467f};
    uint32_t _shadowMapResolution{2048};
    float _shadowDepthBias{1.0f};
    float _shadowNormalBias{1.0f};
    uint32_t _shadowSoftMode{2};
};

template <>
struct RuntimeTypeTrait<DirectionalLightComponent> {
    static constexpr RuntimeTypeId value{0x6f2b1c84, 0x9a3d, 0x4e17, 0xb2, 0x6e, 0x51, 0xa8, 0x7c, 0x0d, 0x33, 0x91};
    using Bases = std::tuple<LightComponent>;
};

}  // namespace radray
