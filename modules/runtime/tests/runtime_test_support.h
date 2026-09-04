#pragma once

#include <atomic>
#include <mutex>

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/window_manager.h>
#include <radray/window/native_window.h>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace radray::test {

inline void CloseMainWindow(Application& app) {
#if defined(_WIN32)
    AppWindow* window = app.GetWindowManager()->GetMainWindow();
    if (window != nullptr) {
        ::PostMessageW(static_cast<HWND>(window->GetNativeWindow()->GetNativeHandler()), WM_CLOSE, 0, 0);
    }
#endif
}

class RuntimeLogCapture {
public:
    RuntimeLogCapture() { SetLogCallback(&Capture, this); }
    ~RuntimeLogCapture() { ClearLogCallback(); }

    string Errors() const {
        std::lock_guard lock{_mutex};
        return _errors;
    }

    std::atomic<uint32_t> IncompatiblePrograms{0};
    std::atomic<uint32_t> DescriptorRewrites{0};

private:
    static void Capture(LogLevel level, std::string_view message, void* userData) {
        auto& self = *static_cast<RuntimeLogCapture*>(userData);
        if (message.find("forward pipeline rejected an incompatible shader program") != std::string_view::npos) {
            ++self.IncompatiblePrograms;
            return;
        }
        if (message.find("rewritten with new buffer targets") != std::string_view::npos) {
            ++self.DescriptorRewrites;
        }
        // Deliberate negative program-cache requests in the existing end-to-end fixture.
        if (message.find("does_not_exist.hlsl") != std::string_view::npos ||
            message.find("duplicate keyword assignment") != std::string_view::npos) {
            return;
        }
        if (level == LogLevel::Err || level == LogLevel::Critical) {
            std::lock_guard lock{self._mutex};
            self._errors.append(message);
            self._errors.push_back('\n');
        }
    }

    mutable std::mutex _mutex;
    string _errors;
};

}  // namespace radray::test
