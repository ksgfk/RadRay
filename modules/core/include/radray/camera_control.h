#pragma once

#include <radray/basic_math.h>

namespace radray {

class CameraControl {
public:
    void Orbit(Eigen::Vector3f& position, Eigen::Quaternionf& rotation) noexcept;
    void PanXY(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept;
    void PanZ(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept;

public:
    Eigen::Vector2f NowPos{};
    Eigen::Vector2f LastPos{};
    float PanDelta{};
    bool CanFly{};
    bool CanOrbit{};
    bool CanPanXY{};
    bool CanPanZWithDist{};
    float Distance{1.0f};
    float MinDistance{0.05f};
    float OrbitSpeed{0.002f};
    float PanXYSpeed{0.002f};
    float PanZSpeed{0.05f};
};

// 一个简单的 FPS 相机控制器：
// - 鼠标控制 yaw/pitch（无 roll）
// - 位移采用局部 yaw 平面（典型 FPS 行走不受 pitch 影响），可选启用自由飞行的垂直移动
// - 以“参数即状态”的方式工作：先写入输入（MouseDelta/MoveXY 等），再调用 Update 应用到 position/rotation
// 约定（左手坐标）：+Z 为前，+X 为右，+Y 为上
class FpsCameraControl {
public:
    // 每帧调用一次，将内部输入参数应用到相机位置与旋转（四元数）
    void Update(Eigen::Vector3f& position, Eigen::Quaternionf& rotation, float deltaTime) noexcept;

    // 将当前 rotation 同步到内部 yaw/pitch（当你外部直接更改 rotation 后可调用）
    void SyncFromRotation(const Eigen::Quaternionf& rotation) noexcept;

public:  // 每帧输入（由上层写入）
    // 鼠标位移（像素或任意未缩放单位，灵敏度由 MouseSensitivity* 负责）
    Eigen::Vector2f MouseDelta{0.0f, 0.0f};

    Eigen::Vector2f MoveXY{0.0f, 0.0f};
    // 垂直移动输入：上(+)/下(-)。仅当 EnableVerticalMove = true 时生效
    float MoveZ{0.0f};

    bool Sprint{false};  // 冲刺
    bool Slow{false};    // 慢走/行走

public:                                // 行为开关
    bool EnableVerticalMove{false};    // 自由飞行（无碰撞）时允许 MoveZ 参与
    bool MoveRelativeToYawOnly{true};  // 平面移动仅相对于 yaw（忽略 pitch）
    bool InvertY{false};               // 鼠标 Y 反转（一般默认 false）

public:                                    // 参数（尽量贴近常见 FPS 手感：Apex/Battlefield 等）
    float MouseSensitivityYaw{0.0017f};    // rad / 像素
    float MouseSensitivityPitch{0.0015f};  // rad / 像素（略小于 yaw，防止竖直过快）

    float MoveSpeed{5.0f};         // m/s 基础移动速度（常见 4.5~5.5）
    float SprintMultiplier{1.6f};  // 冲刺倍率（常见 1.5~1.7）
    float SlowMultiplier{0.5f};    // 慢走倍率（常见 0.4~0.6）
    float VerticalSpeed{5.0f};     // 飞行模式的垂直速度（m/s）

    // 俯仰角范围（单位：弧度） 默认约 [-89°, +89°]
    float PitchMin{Radian(-89.0f)};
    float PitchMax{Radian(+89.0f)};

    // 速度平滑（指数插值）。Damping<=0 表示无平滑，直接采用目标速度
    float Damping{0.0f};  // 建议 8~14 可获得轻微丝滑感；默认 0 贴近竞技类“干脆”手感

private:
    void EnsureInitializedFrom(const Eigen::Quaternionf& rotation) noexcept;

    float _yaw{0.0f};    // 弧度，绕 +Y
    float _pitch{0.0f};  // 弧度，绕 +X（右手法则方向）
    bool _initialized{false};
    Eigen::Vector3f _velocity{0.0f, 0.0f, 0.0f};  // 用于平滑位移
};

}  // namespace radray
