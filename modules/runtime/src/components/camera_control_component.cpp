#include <radray/runtime/components/camera_control_component.h>

#include <cmath>
#include <numbers>

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/window_manager.h>
#include <radray/window/input.h>
#include <radray/window/native_window.h>

#ifdef RADRAY_ENABLE_IMGUI
#include <radray/runtime/imgui_system.h>
#endif

namespace radray {

namespace {

float NormalizeAngle(float angle) noexcept {
    constexpr float PI = std::numbers::pi_v<float>;
    constexpr float TWO_PI = 2.0f * PI;
    angle = std::fmod(angle, TWO_PI);
    if (angle > PI) {
        angle -= TWO_PI;
    }
    if (angle < -PI) {
        angle += TWO_PI;
    }
    return angle;
}

Eigen::Quaternionf MakeCameraRotation(const Eigen::Vector3f& forward, const Eigen::Vector3f& preferredUp = Eigen::Vector3f::UnitY()) noexcept {
    Eigen::Vector3f f = forward.squaredNorm() > 1e-8f ? forward.normalized() : Eigen::Vector3f{0.0f, 0.0f, 1.0f};
    Eigen::Vector3f up = preferredUp.squaredNorm() > 1e-8f ? preferredUp.normalized() : Eigen::Vector3f::UnitY();
    if (up.cross(f).squaredNorm() < 1e-8f) {
        up = std::abs(f.dot(Eigen::Vector3f::UnitY())) < 0.98f ? Eigen::Vector3f::UnitY() : Eigen::Vector3f::UnitZ();
    }
    Eigen::Vector3f right = up.cross(f).normalized();
    Eigen::Vector3f cameraUp = f.cross(right).normalized();
    Eigen::Matrix3f rotation = Eigen::Matrix3f::Identity();
    rotation.col(0) = right;
    rotation.col(1) = cameraUp;
    rotation.col(2) = f;
    Eigen::Quaternionf quat{rotation};
    quat.normalize();
    return quat;
}

Eigen::Quaternionf MakeCameraRotation(float yaw, float pitch) noexcept {
    const float cp = std::cos(pitch);
    const Eigen::Vector3f forward{
        std::sin(yaw) * cp,
        std::sin(pitch),
        std::cos(yaw) * cp};
    return MakeCameraRotation(forward);
}

}  // namespace

CameraControlComponent::CameraControlComponent() noexcept
    : _cameraRotation(MakeCameraRotation(0.0f, Radian(20.0f))) {}

CameraControlComponent::~CameraControlComponent() noexcept {
    UnbindInput();
}

RuntimeTypeId CameraControlComponent::GetTypeId() const noexcept {
    return runtime_type_id_v<CameraControlComponent>;
}

void CameraControlComponent::OnRegister() {
    ResolveCamera();
    ApplyCameraTransform();
}

void CameraControlComponent::OnUnregister() {
    UnbindInput();
    ResetInput();
    _camera = nullptr;
}

void CameraControlComponent::TickComponent(float deltaTime) {
    (void)deltaTime;
    ResolveCamera();
    ApplyInput();
}

void CameraControlComponent::SetCamera(CameraComponent* camera) noexcept {
    _camera = camera;
    ApplyCameraTransform();
}

void CameraControlComponent::BindToWindow(NativeWindow* window) noexcept {
    if (_window == window) {
        return;
    }

    UnbindInput();
    _window = window;
    ResetInput();
    if (_window == nullptr) {
        return;
    }

    _inputConnections.emplace_back(_window->EventTouch().connect([this](int x, int y, MouseButton button, Action action) {
        OnPointer(x, y, button, action);
    }));
    _inputConnections.emplace_back(_window->EventMouseWheel().connect([this](int delta) {
        OnWheel(delta);
    }));
    _inputConnections.emplace_back(_window->EventMouseLeave().connect([this]() {
        ResetInput();
    }));
    _inputConnections.emplace_back(_window->EventFocused().connect([this](bool focused) {
        if (!focused) {
            ResetInput();
        }
    }));
}

void CameraControlComponent::BindToMainWindow(Application& app) noexcept {
    WindowManager* windows = app.GetWindowManager();
    AppWindow* mainWindow = windows != nullptr ? windows->GetMainWindow() : nullptr;
    NativeWindow* nativeWindow = mainWindow != nullptr ? mainWindow->GetNativeWindow() : nullptr;
    if (nativeWindow == nullptr) {
        RADRAY_WARN_LOG("camera control input disabled: no main window");
        BindToWindow(nullptr);
        return;
    }

    BindToWindow(nativeWindow);
#ifdef RADRAY_ENABLE_IMGUI
    SetImGuiSystem(app.GetSubsystem<ImGuiSystem>());
#else
    (void)app;
#endif
}

void CameraControlComponent::UnbindInput() noexcept {
    _inputConnections.clear();
    _window = nullptr;
}

void CameraControlComponent::SetOrbitTarget(const Eigen::Vector3f& target) noexcept {
    _target = target;
    _control.SetOrbitTarget(_target);
    ApplyCameraTransform();
}

void CameraControlComponent::SetFrame(const Eigen::Vector3f& target, float distance, float yaw, float pitch) noexcept {
    _control.Reset();
    _target = target;
    _distance = Clamp(distance, _control.MinDistance, _control.MaxDistance);
    _cameraRotation = MakeCameraRotation(yaw, NormalizeAngle(pitch));
    _control.SetOrbitTarget(_target);
    ApplyCameraTransform();
}

void CameraControlComponent::ResolveCamera() noexcept {
    if (_camera != nullptr) {
        return;
    }

    Nullable<Actor*> owner = GetOwner();
    if (!owner) {
        return;
    }
    _camera = owner.Get()->FindComponent<CameraComponent>();
}

bool CameraControlComponent::IsUiBlockingCameraInput() const {
#ifdef RADRAY_ENABLE_IMGUI
    if (_imgui == nullptr || !_imgui->IsInitialized() || !_imgui->IsValid()) {
        return false;
    }
    if (ImGui::GetCurrentContext() == nullptr) {
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureMouse ||
           ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ||
           ImGui::IsAnyItemActive();
#else
    return false;
#endif
}

bool CameraControlComponent::IsPointerInsideWindow(const Eigen::Vector2f& mousePos) const noexcept {
    if (_window == nullptr) {
        return false;
    }
    const Eigen::Vector2i size = _window->GetSize();
    return mousePos.x() >= 0.0f &&
           mousePos.y() >= 0.0f &&
           mousePos.x() < static_cast<float>(size.x()) &&
           mousePos.y() < static_cast<float>(size.y());
}

void CameraControlComponent::ApplyInput() {
    if (_window == nullptr) {
        ResetInput();
        return;
    }

    const bool uiBlockingCamera = IsUiBlockingCameraInput();
    if (std::abs(_wheelDelta) > 1e-6f) {
        if (!uiBlockingCamera && IsPointerInsideWindow(_mousePos)) {
            const float move = _wheelDelta * _control.DollySensitivity * _distance;
            _distance = Clamp(_distance - move, _control.MinDistance, _control.MaxDistance);
            ApplyCameraTransform();
        }
        _wheelDelta = 0.0f;
    }

    const bool anyDown = _leftDown || _rightDown || _middleDown;
    if (!anyDown) {
        _anyButtonDown = false;
        _inputCapturedByUi = false;
        _control.IsOrbiting = false;
        _control.IsPanning = false;
        _control.IsDollying = false;
        _control.LastMousePos = _mousePos;
        _lastMousePos = _mousePos;
        return;
    }

    if (!_anyButtonDown || _dragStartPending) {
        _anyButtonDown = true;
        _dragStartPending = false;
        _inputCapturedByUi = !IsPointerInsideWindow(_mousePos) || uiBlockingCamera;
        _control.LastMousePos = _mousePos;
        _lastMousePos = _mousePos;
    }

    const bool pan = _rightDown || _middleDown;
    const bool orbit = _leftDown && !pan;
    _control.IsOrbiting = orbit && !_inputCapturedByUi;
    _control.IsPanning = pan && !_inputCapturedByUi;
    const Eigen::Vector2f delta = _mousePos - _lastMousePos;
    _lastMousePos = _mousePos;
    _control.CurrentMousePos = _mousePos;

    if (_inputCapturedByUi) {
        _control.IsOrbiting = false;
        _control.IsPanning = false;
        _control.IsDollying = false;
        _control.LastMousePos = _mousePos;
        return;
    }

    if (delta.squaredNorm() < 1e-6f) {
        return;
    }

    if (orbit) {
        Eigen::Quaternionf deltaRotation = Eigen::Quaternionf::Identity();
        if (_control.UseTrackball) {
            const Eigen::Vector3f axis{-delta.y(), delta.x(), 0.0f};
            const float angle = NormalizeAngle(delta.norm() * _control.OrbitSensitivity);
            if (axis.squaredNorm() > 1e-8f && std::abs(angle) > 1e-6f) {
                const Eigen::Vector3f worldAxis = _cameraRotation * axis.normalized();
                deltaRotation = Eigen::Quaternionf{Eigen::AngleAxisf{angle, worldAxis}};
            }
        } else {
            const float yawAngle = NormalizeAngle(delta.x() * _control.OrbitSensitivity);
            const float pitchAngle = NormalizeAngle(delta.y() * _control.OrbitSensitivity);
            const Eigen::Vector3f up = _cameraRotation * Eigen::Vector3f::UnitY();
            const Eigen::Quaternionf yawRotation{Eigen::AngleAxisf{yawAngle, up.normalized()}};
            const Eigen::Vector3f right = _cameraRotation * Eigen::Vector3f::UnitX();
            const Eigen::Quaternionf pitchRotation{Eigen::AngleAxisf{pitchAngle, right.normalized()}};
            deltaRotation = yawRotation * pitchRotation;
        }
        _cameraRotation = deltaRotation * _cameraRotation;
        _cameraRotation.normalize();
        ApplyCameraTransform();
    } else if (pan) {
        const Eigen::Vector3f right = _cameraRotation * Eigen::Vector3f::UnitX();
        const Eigen::Vector3f up = _cameraRotation * Eigen::Vector3f::UnitY();
        const float scale = _distance * _control.PanSensitivity * 0.5f;
        const Eigen::Vector3f panVector = right * (delta.x() * scale) + up * (delta.y() * scale);
        _target += panVector;
        _control.SetOrbitTarget(_target);
        ApplyCameraTransform();
    }
}

void CameraControlComponent::ApplyCameraTransform() noexcept {
    if (_camera == nullptr) {
        return;
    }

    const Eigen::Vector3f dir = _cameraRotation * Eigen::Vector3f::UnitZ();
    const Eigen::Vector3f cameraPosition = _target - dir * _distance;
    _control.SetOrbitTarget(_target);
    _control.UpdateDistance(cameraPosition);
    _camera->SetWorldLocation(cameraPosition);
    _camera->SetWorldRotation(_cameraRotation);
}

void CameraControlComponent::ResetInput() noexcept {
    _control.IsOrbiting = false;
    _control.IsPanning = false;
    _control.IsDollying = false;
    _control.WheelDelta = 0.0f;
    _inputCapturedByUi = false;
    _dragStartPending = false;
    _anyButtonDown = false;
    _leftDown = false;
    _rightDown = false;
    _middleDown = false;
    _wheelDelta = 0.0f;
}

void CameraControlComponent::SetButtonDown(MouseButton button, bool down) noexcept {
    if (button == MouseButton::BUTTON_LEFT) {
        _leftDown = down;
    } else if (button == MouseButton::BUTTON_RIGHT) {
        _rightDown = down;
    } else if (button == MouseButton::BUTTON_MIDDLE) {
        _middleDown = down;
    }
}

void CameraControlComponent::OnPointer(int x, int y, MouseButton button, Action action) {
    if (action == Action::UNKNOWN) {
        return;
    }

    _mousePos = Eigen::Vector2f{static_cast<float>(x), static_cast<float>(y)};
    _control.CurrentMousePos = _mousePos;

    if (action == Action::PRESSED) {
        SetButtonDown(button, true);
        _dragStartPending = true;
        _control.LastMousePos = _mousePos;
        _lastMousePos = _mousePos;
        return;
    }

    if (action == Action::RELEASED) {
        SetButtonDown(button, false);
        _control.LastMousePos = _mousePos;
        _lastMousePos = _mousePos;
        if (!_leftDown && !_rightDown && !_middleDown) {
            _inputCapturedByUi = false;
            _dragStartPending = false;
        }
    }
}

void CameraControlComponent::OnWheel(int delta) noexcept {
    _wheelDelta += static_cast<float>(delta) / 120.0f;
}

}  // namespace radray
