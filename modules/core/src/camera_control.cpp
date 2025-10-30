#include <radray/camera_control.h>

namespace radray {

void CameraControl::Orbit(Eigen::Vector3f& position, Eigen::Quaternionf& rotation) noexcept {
    if (CanFly) {
        Eigen::Vector3f right = (rotation * Eigen::Vector3f{1, 0, 0}).normalized();
        Eigen::Vector2f delta = NowPos - LastPos;
        Eigen::AngleAxisf rotX(delta.x() * OrbitSpeed, Eigen::Vector3f::UnitY());
        Eigen::AngleAxisf rotY(delta.y() * OrbitSpeed, right);
        rotation = rotX * rotY * rotation;
    }
    if (CanOrbit) {
        Eigen::Vector3f pos = position;
        Eigen::Vector3f front = (rotation * Eigen::Vector3f{0, 0, 1}).normalized();
        Eigen::Vector3f right = (rotation * Eigen::Vector3f{1, 0, 0}).normalized();
        Eigen::Vector3f target = pos + front * Distance;
        Eigen::Vector2f delta = NowPos - LastPos;
        Eigen::Vector3f local = pos - target;
        Eigen::AngleAxisf rotX(delta.x() * OrbitSpeed, Eigen::Vector3f::UnitY());
        Eigen::AngleAxisf rotY(delta.y() * OrbitSpeed, right);
        local = rotX * rotY * local;
        position = local + target;
        rotation = rotX * rotY * rotation;
    }
    if (CanFly || CanOrbit) {
        LastPos = NowPos;
    }
}

void CameraControl::PanXY(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept {
    if (CanPanXY) {
        Eigen::Vector3f right = (rotation * Eigen::Vector3f{1, 0, 0}).normalized();
        Eigen::Vector3f up = (rotation * Eigen::Vector3f{0, 1, 0}).normalized();
        Eigen::Vector2f delta = NowPos - LastPos;
        Eigen::Vector3f pos = position;
        float speed = PanXYSpeed * Distance * 0.35f;
        position = pos + up * (delta.y() * speed) + right * (-delta.x() * speed);
    }
    if (CanPanXY) {
        LastPos = NowPos;
    }
}

void CameraControl::PanZ(Eigen::Vector3f& position, const Eigen::Quaternionf& rotation) noexcept {
    float dist;
    if (CanPanZWithDist) {
        dist = PanDelta * PanZSpeed;
    } else {
        if (PanDelta > 0) {
            if (Distance > MinDistance) {
                dist = std::min(PanDelta * PanZSpeed, Distance - MinDistance);
                dist *= Distance;
                Distance = std::max(MinDistance, Distance - dist);
            } else {
                dist = 0;
                Distance = MinDistance;
            }
        } else {
            dist = PanDelta * PanZSpeed;
            dist *= Distance;
            Distance = Distance - dist;
        }
    }
    Eigen::Vector3f front = (rotation * Eigen::Vector3f{0, 0, 1}).normalized();
    Eigen::Vector3f pos = position;
    position = pos + front * dist;
}

static float WrapAnglePi(float a) noexcept {
    // wrap to [-pi, pi)
    using std::numbers::pi_v;
    const float twoPi = 2.0f * pi_v<float>;
    a = std::fmod(a + pi_v<float>, twoPi);
    if (a < 0.0f) a += twoPi;
    return a - pi_v<float>;
}

void FpsCameraControl::EnsureInitializedFrom(const Eigen::Quaternionf& rotation) noexcept {
    if (_initialized) return;
    // 从 rotation 推导 yaw/pitch。前向 = rot * (0,0,1)
    Eigen::Vector3f f = (rotation * Eigen::Vector3f{0, 0, 1}).normalized();
    // 基于左手坐标的常见分解：
    // f = { cos(pitch)*sin(yaw), sin(pitch), cos(pitch)*cos(yaw) }
    _pitch = std::asin(radray::Clamp(f.y(), -1.0f, 1.0f));
    _yaw = std::atan2(f.x(), f.z());
    _yaw = WrapAnglePi(_yaw);
    _pitch = radray::Clamp(_pitch, PitchMin, PitchMax);
    _initialized = true;
}

void FpsCameraControl::SyncFromRotation(const Eigen::Quaternionf& rotation) noexcept {
    _initialized = false;
    EnsureInitializedFrom(rotation);
}

void FpsCameraControl::Update(Eigen::Vector3f& position, Eigen::Quaternionf& rotation, float deltaTime) noexcept {
    EnsureInitializedFrom(rotation);

    // 1) 处理鼠标旋转
    float dyaw = MouseDelta.x() * MouseSensitivityYaw;
    float dpitch = MouseDelta.y() * MouseSensitivityPitch * (InvertY ? 1.0f : -1.0f);
    _yaw = WrapAnglePi(_yaw + dyaw);
    _pitch = radray::Clamp(_pitch + dpitch, PitchMin, PitchMax);

    Eigen::AngleAxisf qYaw(_yaw, Eigen::Vector3f::UnitY());
    Eigen::AngleAxisf qPitch(_pitch, Eigen::Vector3f::UnitX());
    rotation = qYaw * qPitch;  // yaw 后接 pitch（无 roll）

    // 2) 计算目标速度
    float speed = MoveSpeed;
    if (Sprint) speed *= SprintMultiplier;
    if (Slow) speed *= SlowMultiplier;

    // 平面方向（相对 yaw 或完整旋转）
    Eigen::Vector3f forward, right;
    if (MoveRelativeToYawOnly) {
        forward = (qYaw * Eigen::Vector3f{0, 0, 1}).normalized();
        right = (qYaw * Eigen::Vector3f{1, 0, 0}).normalized();
    } else {
        Eigen::Quaternionf q = rotation;
        forward = (q * Eigen::Vector3f{0, 0, 1}).normalized();
        right = (q * Eigen::Vector3f{1, 0, 0}).normalized();
    }

    Eigen::Vector3f desired = Eigen::Vector3f::Zero();
    if (MoveXY.squaredNorm() > 0.0f) {
        desired += forward * MoveXY.y();
        desired += right * MoveXY.x();
    }
    if (desired.squaredNorm() > 0.0f) desired.normalize();
    desired *= speed;

    if (EnableVerticalMove && std::abs(MoveZ) > 0.0f) {
        desired += Eigen::Vector3f{0, 1, 0} * (MoveZ * VerticalSpeed);
    }

    // 3) 速度平滑（指数插值）。Damping<=0 表示直接采用目标速度
    Eigen::Vector3f currV = _velocity;
    if (Damping > 0.0f) {
        float alpha = 1.0f - std::exp(-Damping * std::max(0.0f, deltaTime));
        currV = currV + (desired - currV) * radray::Clamp(alpha, 0.0f, 1.0f);
    } else {
        currV = desired;
    }

    // 4) 移动 & 写回
    position += currV * std::max(0.0f, deltaTime);
    _velocity = currV;

    // 清理本帧输入（交由上层每帧写入）
    MouseDelta.setZero();
    // MoveXY/MoveZ 通常由键盘持续驱动，不在此清空
}

}  // namespace radray
