#include "d3d12_helper.h"

namespace radray::render::d3d12 {

Win32Event::Win32Event(Win32Event&& other) noexcept
    : _event(other._event) {
    other._event = nullptr;
}

Win32Event& Win32Event::operator=(Win32Event&& other) noexcept {
    if (this != &other) {
        _event = other._event;
        other._event = nullptr;
    }
    return *this;
}

Win32Event::~Win32Event() noexcept {
    Destroy();
}

void Win32Event::Destroy() noexcept {
    if (_event) {
        CloseHandle(_event);
        _event = nullptr;
    }
}

std::optional<Win32Event> MakeWin32Event() noexcept {
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (event == nullptr) {
        DWORD err = GetLastError();
        RADRAY_ERR_LOG("cannot create WIN32 event, code:{}", err);
        return std::nullopt;
    }
    Win32Event result{};
    result._event = event;
    return std::make_optional(std::move(result));
}

}  // namespace radray::render::d3d12
