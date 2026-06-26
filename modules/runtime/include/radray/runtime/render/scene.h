#pragma once

#include <cstdint>
#include <span>

#include <radray/types.h>
#include <radray/basic_math.h>

namespace radray::srp {

class Renderer;

/// 光源类型(对应旧 radray::LightType,迁入 srp 命名空间)。
enum class LightType : uint32_t {
    Directional,
    Point,
    Spot,
};

/// 一盏灯的世界空间参数(纯数据)。替代旧 LightSceneProxy。
/// per-view space0 构建器据此填充 GPU 灯光缓冲。
struct Light {
    LightType Type{LightType::Point};
    Eigen::Vector3f Direction{0.0f, -1.0f, 0.0f};  ///< 方向(directional/spot)
    Eigen::Vector3f Color{1.0f, 1.0f, 1.0f};
    Eigen::Vector3f Position{Eigen::Vector3f::Zero()};
    float Intensity{1.0f};
    float Range{10.0f};                  ///< point/spot 影响半径,<=0 关闭衰减截断
    float SpotInnerAngle{0.5235988f};    ///< 30°
    float SpotOuterAngle{0.6981317f};    ///< 40°
    bool CastShadow{true};
    float ShadowDepthBias{1.0f};
    float ShadowNormalBias{1.0f};
};

/// 渲染侧世界注册表(对应 Unity 的可见对象集合 / UE5 FScene)。
/// 持有 Renderer*(借用,组件拥有实体)与 Light(值)。CullAll 收集全部可渲染对象。
///
/// 最小化:不做 frustum culling;不拥有 Renderer 生命周期(组件在注册/注销时增删)。
class Scene {
public:
    Scene() = default;
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // —— Renderer 注册(借用指针,Scene 不拥有)——
    void AddRenderer(Renderer* renderer) {
        if (renderer != nullptr) {
            _renderers.push_back(renderer);
        }
    }
    void RemoveRenderer(Renderer* renderer) noexcept {
        for (auto it = _renderers.begin(); it != _renderers.end(); ++it) {
            if (*it == renderer) {
                _renderers.erase(it);
                return;
            }
        }
    }
    std::span<Renderer* const> Renderers() const noexcept { return _renderers; }
    size_t RendererCount() const noexcept { return _renderers.size(); }

    // —— Light 注册(Scene 拥有 unique_ptr,组件持稳定 Light* 句柄)——
    Light* AddLight() {
        _lights.push_back(make_unique<Light>());
        return _lights.back().get();
    }
    void RemoveLight(const Light* light) noexcept {
        for (auto it = _lights.begin(); it != _lights.end(); ++it) {
            if (it->get() == light) {
                _lights.erase(it);
                return;
            }
        }
    }
    std::span<const unique_ptr<Light>> Lights() const noexcept { return _lights; }
    size_t LightCount() const noexcept { return _lights.size(); }

    void Clear() noexcept {
        _renderers.clear();
        _lights.clear();
    }

private:
    vector<Renderer*> _renderers;
    vector<unique_ptr<Light>> _lights;
};

}  // namespace radray::srp
