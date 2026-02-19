#pragma once

#include <atomic>

#include <radray/window/native_window.h>

#ifdef __OBJC__
@class NSWindow;
@class NSView;
@class RadrayWindowDelegate;
#endif

namespace radray {

class CocoaWindow : public NativeWindow {
public:
    CocoaWindow() noexcept;
    ~CocoaWindow() noexcept override;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

    void DispatchEvents() noexcept override;

    bool ShouldClose() const noexcept override;
    WindowNativeHandler GetNativeHandler() const noexcept override;
    WindowVec2i GetSize() const noexcept override;
    bool IsMinimized() const noexcept override;

    void SetSize(int width, int height) noexcept override;

    sigslot::signal<int, int>& EventResized() noexcept override;
    sigslot::signal<int, int>& EventResizing() noexcept override;
    sigslot::signal<int, int, MouseButton, Action>& EventTouch() noexcept override;
    sigslot::signal<KeyCode, Action>& EventKeyboard() noexcept override;
    sigslot::signal<int>& EventMouseWheel() noexcept override;

public:
    void DestroyImpl() noexcept;

#ifdef __OBJC__
    NSWindow* _nsWindow{nullptr};
    NSView* _nsView{nullptr};
    RadrayWindowDelegate* _windowDelegate{nullptr};
#endif
    std::atomic_bool _closeRequested{false};

    sigslot::signal<int, int> _eventResized;
    sigslot::signal<int, int> _eventResizing;
    sigslot::signal<int, int, MouseButton, Action> _eventTouch;
    sigslot::signal<KeyCode, Action> _eventKeyboard;
    sigslot::signal<int> _eventMouseWheel;
};

Nullable<unique_ptr<CocoaWindow>> CreateCocoaWindow(const CocoaWindowCreateDescriptor& desc) noexcept;

}  // namespace radray
