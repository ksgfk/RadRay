#pragma once

#include <sigslot/signal.hpp>

#include <radray/basic_math.h>
#include <radray/camera_control.h>
#include <radray/runtime/components/actor_component.h>

namespace radray {

class Application;
class CameraComponent;
class ImGuiSystem;
class NativeWindow;
enum class Action;
enum class MouseButton;

/// Orbit/pan/dolly controller for a CameraComponent owned by the same Actor.
/// Bind it to a window for pointer input, then TickComponent applies the result.
class CameraControlComponent : public ActorComponent {
public:
    CameraControlComponent() noexcept;
    ~CameraControlComponent() noexcept override;

    RuntimeTypeId GetTypeId() const noexcept override;

    void OnRegister() override;
    void OnUnregister() override;
    void TickComponent(float deltaTime) override;

    CameraControl& GetControl() noexcept { return _control; }
    const CameraControl& GetControl() const noexcept { return _control; }

    void SetCamera(CameraComponent* camera) noexcept;
    CameraComponent* GetCamera() const noexcept { return _camera; }

    void BindToWindow(NativeWindow* window) noexcept;
    void BindToMainWindow(Application& app) noexcept;
    void SetImGuiSystem(ImGuiSystem* imgui) noexcept { _imgui = imgui; }
    void UnbindInput() noexcept;

    void SetOrbitTarget(const Eigen::Vector3f& target) noexcept;
    const Eigen::Vector3f& GetOrbitTarget() const noexcept { return _target; }
    float GetDistance() const noexcept { return _distance; }
    void SetFrame(const Eigen::Vector3f& target, float distance, float yaw = 0.0f, float pitch = Radian(20.0f)) noexcept;

private:
    void ResolveCamera() noexcept;
    bool IsUiBlockingCameraInput() const;
    bool IsPointerInsideWindow(const Eigen::Vector2f& mousePos) const noexcept;
    void ApplyInput();
    void ApplyCameraTransform() noexcept;
    void ResetInput() noexcept;
    void SetButtonDown(MouseButton button, bool down) noexcept;
    void OnPointer(int x, int y, MouseButton button, Action action);
    void OnWheel(int delta) noexcept;

    CameraControl _control;
    CameraComponent* _camera{nullptr};
    NativeWindow* _window{nullptr};
    ImGuiSystem* _imgui{nullptr};
    vector<sigslot::scoped_connection> _inputConnections;

    Eigen::Vector3f _target{Eigen::Vector3f::Zero()};
    float _distance{4.0f};
    Eigen::Quaternionf _cameraRotation{Eigen::Quaternionf::Identity()};

    float _wheelDelta{0.0f};
    Eigen::Vector2f _mousePos{Eigen::Vector2f::Zero()};
    Eigen::Vector2f _lastMousePos{Eigen::Vector2f::Zero()};
    bool _inputCapturedByUi{false};
    bool _dragStartPending{false};
    bool _anyButtonDown{false};
    bool _leftDown{false};
    bool _rightDown{false};
    bool _middleDown{false};
};

template <>
struct RuntimeTypeTrait<CameraControlComponent> {
    static constexpr RuntimeTypeId value{0xe605b87f, 0x7f2d, 0x497e, 0xb4, 0xc4, 0x38, 0xbe, 0x29, 0x6f, 0x94, 0xe1};
};

}  // namespace radray
