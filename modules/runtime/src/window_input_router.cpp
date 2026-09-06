#include <radray/runtime/window_input_router.h>
#include <atomic>

namespace radray {
WindowInputRouter::WindowInputRouter(NativeWindow& window) {
    _connections.push_back(window.EventTouch().connect([this](int x, int y, MouseButton button, Action state) {
        WindowInputEvent event;
        event.Type = state == Action::UNKNOWN || state == Action::REPEATED ? WindowInputType::Move : WindowInputType::Button;
        event.Position = {float(x), float(y)};
        event.Button = button;
        event.State = state;
        Push(std::move(event));
    }));
    _connections.push_back(window.EventKeyboard().connect([this](KeyCode key, Action state) {
        WindowInputEvent event;
        event.Type = WindowInputType::Key;
        event.Key = key;
        event.State = state;
        Push(std::move(event));
    }));
    _connections.push_back(window.EventScroll().connect([this](float x, float y) {
        WindowInputEvent event;
        event.Type = WindowInputType::Scroll;
        event.Position = {x, y};
        Push(std::move(event));
    }));
    _connections.push_back(window.EventTextInput().connect([this](std::string_view text) {
        WindowInputEvent event;
        event.Type = WindowInputType::Text;
        event.Text = text;
        Push(std::move(event));
    }));
    _connections.push_back(window.EventFocused().connect([this](bool focused) {
        WindowInputEvent event;
        event.Type = WindowInputType::Focus;
        event.Focused = focused;
        Push(std::move(event));
    }));
    _connections.push_back(window.EventCaptureLost().connect([this] { WindowInputEvent event; event.Type = WindowInputType::CaptureLost; Push(std::move(event)); }));
    _connections.push_back(window.EventCloseRequested().connect([this] { WindowInputEvent event; event.Type = WindowInputType::Focus; Push(std::move(event)); }));
    _connections.push_back(window.EventMouseLeave().connect([this] { WindowInputEvent event; event.Type = WindowInputType::Leave; Push(std::move(event)); }));
}
void WindowInputRouter::Push(WindowInputEvent event) {
    static std::atomic_uint64_t sequence{1};
    event.Sequence = sequence.fetch_add(1, std::memory_order_relaxed);
    _pending.push_back(std::move(event));
}
void WindowInputRouter::CancelButtons() {
    auto buttons = std::move(_buttons);
    _buttons.clear();
    for (const auto [button, application] : buttons)
        if (application) {
            WindowInputEvent event;
            event.Type = WindowInputType::Button;
            event.Button = button;
            event.State = Action::RELEASED;
            _eventInput(event);
        }
}
void WindowInputRouter::Cancel() {
    auto keys = std::move(_keys);
    _keys.clear();
    for (const auto [key, application] : keys)
        if (application) {
            WindowInputEvent event;
            event.Type = WindowInputType::Key;
            event.Key = key;
            event.State = Action::RELEASED;
            _eventInput(event);
        }
    CancelButtons();
}
void WindowInputRouter::Dispatch() {
    vector<WindowInputEvent> events;
    events.swap(_pending);
    for (const auto& event : events) {
        bool deliver = _applicationEnabled;
        if (event.Type == WindowInputType::Key) {
            if (event.State == Action::PRESSED) _keys.try_emplace(event.Key, deliver && !_captureKeyboard);
            const auto owner = _keys.find(event.Key);
            deliver = owner != _keys.end() && owner->second;
            if (event.State == Action::RELEASED && owner != _keys.end()) _keys.erase(owner);
        } else if (event.Type == WindowInputType::Button) {
            if (event.State == Action::PRESSED) _buttons.try_emplace(event.Button, deliver && !_captureMouse);
            const auto owner = _buttons.find(event.Button);
            deliver = owner != _buttons.end() && owner->second;
            if (event.State == Action::RELEASED && owner != _buttons.end()) _buttons.erase(owner);
        } else if (event.Type == WindowInputType::Focus && !event.Focused) {
            Cancel();
        } else if (event.Type == WindowInputType::CaptureLost) {
            CancelButtons();
        } else if (event.Type == WindowInputType::Move) {
            bool held = false;
            for (const auto [button, application] : _buttons) {
                (void)button;
                held |= application;
            }
            deliver &= held || !_captureMouse;
        } else if (event.Type == WindowInputType::Scroll)
            deliver &= !_captureMouse;
        else if (event.Type == WindowInputType::Text)
            deliver &= !_captureKeyboard;
        if (deliver) _eventInput(event);
    }
}
}  // namespace radray
