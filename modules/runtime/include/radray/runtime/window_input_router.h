#pragma once

#include <radray/window/native_window.h>

namespace radray {

enum class WindowInputType : uint8_t { Move,
                                       Button,
                                       Key,
                                       Scroll,
                                       Text,
                                       Focus,
                                       CaptureLost,
                                       Leave };
struct WindowInputEvent {
    WindowInputType Type{WindowInputType::Move};
    Eigen::Vector2f Position{0, 0};
    MouseButton Button{MouseButton::UNKNOWN};
    KeyCode Key{KeyCode::UNKNOWN};
    Action State{Action::UNKNOWN};
    string Text{};
    bool Focused{false};
    uint64_t Sequence{0};
};

/// Raw events are copied until the writable update phase. Key/button release follows its press owner.
class WindowInputRouter {
public:
    WindowInputRouter() = default;
    explicit WindowInputRouter(NativeWindow& window);
    void Push(WindowInputEvent event);
    std::span<const WindowInputEvent> Pending() const noexcept { return _pending; }
    void SetCapture(bool mouse, bool keyboard) noexcept {
        _captureMouse = mouse;
        _captureKeyboard = keyboard;
    }
    void SetApplicationEnabled(bool enabled) {
        if (_applicationEnabled && !enabled) Cancel();
        _applicationEnabled = enabled;
    }
    void Dispatch();
    void Cancel();
    sigslot::signal<const WindowInputEvent&>& EventInput() noexcept { return _eventInput; }

private:
    void CancelButtons();
    vector<sigslot::scoped_connection> _connections;
    vector<WindowInputEvent> _pending;
    unordered_map<KeyCode, bool> _keys;
    unordered_map<MouseButton, bool> _buttons;
    sigslot::signal<const WindowInputEvent&> _eventInput;
    bool _captureMouse{false}, _captureKeyboard{false}, _applicationEnabled{true};
};

}  // namespace radray
