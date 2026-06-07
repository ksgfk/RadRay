#include <radray/window/cocoa/cocoa_window.h>

#ifdef RADRAY_PLATFORM_MACOS

#import <radray/window/cocoa/cocoa_window_objc.h>

#import <Carbon/Carbon.h>

#include <radray/logger.h>

#include <algorithm>
#include <cmath>

@interface RadrayCocoaWindowBridgeDelegate : NSObject <RadrayCocoaWindowDelegate>
- (instancetype)initWithWindow:(radray::CocoaWindow*)window;
@property(nonatomic, assign) radray::CocoaWindow* window;
@end

namespace radray {

static RadrayCocoaWindow* ToObjCWindow(void* window) noexcept {
    return (__bridge RadrayCocoaWindow*)window;
}

static RadrayCocoaWindowBridgeDelegate* ToObjCDelegate(void* delegate) noexcept {
    return (__bridge RadrayCocoaWindowBridgeDelegate*)delegate;
}

static RadrayCocoaEventPump* ToObjCEventPump(void* pump) noexcept {
    return (__bridge RadrayCocoaEventPump*)pump;
}

static void* RetainObjCObject(id object) noexcept {
    return (__bridge_retained void*)object;
}

static void ReleaseObjCObject(void*& object) noexcept {
    if (object != nullptr) {
        id objectToRelease = CFBridgingRelease(object);
        (void)objectToRelease;
        object = nullptr;
    }
}

static RadrayCocoaWindow* CocoaWindowObjC(const CocoaWindow* window) noexcept {
    return window != nullptr ? ToObjCWindow(window->_objcWindow) : nil;
}

static NSString* ToNSString(std::string_view str) noexcept {
    return [[NSString alloc] initWithBytes:str.data() length:str.size() encoding:NSUTF8StringEncoding];
}

static RadrayCocoaWindowShowMode ToCocoaShowMode(NativeWindowShowMode mode) noexcept {
    switch (mode) {
        case NativeWindowShowMode::Default: return RadrayCocoaWindowShowModeDefault;
        case NativeWindowShowMode::NoActivate: return RadrayCocoaWindowShowModeNoActivate;
    }
    return RadrayCocoaWindowShowModeDefault;
}

static Action ToAction(RadrayCocoaWindowAction action) noexcept {
    switch (action) {
        case RadrayCocoaWindowActionReleased: return Action::RELEASED;
        case RadrayCocoaWindowActionPressed: return Action::PRESSED;
        case RadrayCocoaWindowActionRepeated: return Action::REPEATED;
        case RadrayCocoaWindowActionUnknown:
        default: return Action::UNKNOWN;
    }
}

static MouseButton ToMouseButton(RadrayCocoaMouseButton button) noexcept {
    switch (button) {
        case RadrayCocoaMouseButton1: return MouseButton::BUTTON_1;
        case RadrayCocoaMouseButton2: return MouseButton::BUTTON_2;
        case RadrayCocoaMouseButton3: return MouseButton::BUTTON_3;
        case RadrayCocoaMouseButton4: return MouseButton::BUTTON_4;
        case RadrayCocoaMouseButton5: return MouseButton::BUTTON_5;
        case RadrayCocoaMouseButton6: return MouseButton::BUTTON_6;
        case RadrayCocoaMouseButton7: return MouseButton::BUTTON_7;
        case RadrayCocoaMouseButton8: return MouseButton::BUTTON_8;
        case RadrayCocoaMouseButtonUnknown:
        default: return MouseButton::UNKNOWN;
    }
}

static int ToInt(CGFloat value) noexcept {
    if (!std::isfinite(static_cast<double>(value))) {
        return 0;
    }
    return static_cast<int>(std::lround(static_cast<double>(value)));
}

static RadrayCocoaWindow* ExtractCocoaOwner(Nullable<NativeWindow*> owner) noexcept {
    if (!owner) {
        return nil;
    }

    NativeWindow* ownerWindow = owner.Get();
    if (ownerWindow == nullptr) {
        return nil;
    }
    if (ownerWindow->GetType() != NativeWindowType::CocoaNSWindow) {
        RADRAY_ERR_LOG("CocoaWindow owner must be a CocoaNSWindow window");
        return nil;
    }

    auto* cocoaOwner = static_cast<CocoaWindow*>(ownerWindow);
    return CocoaWindowObjC(cocoaOwner);
}

static RadrayCocoaWindowDescriptor ToCocoaDescriptor(const CocoaWindowCreateDescriptor& desc) noexcept {
    RadrayCocoaWindowDescriptor result = RadrayCocoaWindowDescriptorMakeDefault();
    result.title = ToNSString(desc.Title);
    result.ownerWindow = ExtractCocoaOwner(desc.OwnerWindow);
    result.width = static_cast<CGFloat>(desc.Width);
    result.height = static_cast<CGFloat>(desc.Height);
    result.x = static_cast<CGFloat>(desc.X);
    result.y = static_cast<CGFloat>(desc.Y);
    result.resizable = desc.Resizable;
    result.startMaximized = desc.StartMaximized;
    result.fullscreen = desc.Fullscreen;
    result.startVisible = desc.StartVisible;
    result.decorated = desc.Decorated;
    result.showInTaskbar = desc.ShowInTaskbar;
    result.topMost = desc.TopMost;
    result.activateOnShow = desc.ActivateOnShow;
    result.focusOnClick = desc.FocusOnClick;
    result.inputPassthrough = desc.InputPassthrough;
    return result;
}

CocoaEventPump::CocoaEventPump() noexcept {
    _pump = RetainObjCObject([[RadrayCocoaEventPump alloc] init]);
}

CocoaEventPump::~CocoaEventPump() noexcept {
    ReleaseObjCObject(_pump);
}

void CocoaEventPump::DispatchEvents() noexcept {
    [ToObjCEventPump(_pump) dispatchEvents];
}

bool CocoaEventPump::Register(NativeWindow* window) noexcept {
    if (window == nullptr || window->GetType() != NativeWindowType::CocoaNSWindow) {
        return false;
    }
    if (std::find(_windows.begin(), _windows.end(), window) != _windows.end()) {
        return true;
    }

    auto* cocoaWindow = static_cast<CocoaWindow*>(window);
    RadrayCocoaWindow* objcWindow = (__bridge RadrayCocoaWindow*)cocoaWindow->GetObjCWindow();
    if (objcWindow == nil) {
        return false;
    }
    if (![ToObjCEventPump(_pump) registerWindow:objcWindow]) {
        return false;
    }

    _windows.push_back(window);
    return true;
}

void CocoaEventPump::Unregister(NativeWindow* window) noexcept {
    auto iter = std::find(_windows.begin(), _windows.end(), window);
    if (iter == _windows.end()) {
        return;
    }

    auto* cocoaWindow = static_cast<CocoaWindow*>(window);
    RadrayCocoaWindow* objcWindow = (__bridge RadrayCocoaWindow*)cocoaWindow->GetObjCWindow();
    if (objcWindow != nil) {
        [ToObjCEventPump(_pump) unregisterWindow:objcWindow];
    }
    _windows.erase(iter);
}

sigslot::signal<NativeWindow*>& CocoaEventPump::EventModalLoopTick() noexcept {
    return _eventModalLoopTick;
}

CocoaWindow::CocoaWindow() noexcept = default;

CocoaWindow::~CocoaWindow() noexcept {
    Destroy();
}

bool CocoaWindow::IsValid() const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    return window != nil && [window isValid];
}

void CocoaWindow::Destroy() noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        ToObjCDelegate(_objcDelegate).window = nullptr;
        window.delegate = nil;
        [window destroy];
    }
    ReleaseObjCObject(_objcWindow);
    ReleaseObjCObject(_objcDelegate);
}

void CocoaWindow::Show() noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window show];
    }
}

void CocoaWindow::Show(NativeWindowShowMode mode) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window showWithMode:ToCocoaShowMode(mode)];
    }
}

void CocoaWindow::Focus() noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window focus];
    }
}

bool CocoaWindow::ShouldClose() const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    return window != nil && [window shouldClose];
}

void* CocoaWindow::GetNativeHandler() const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    return window != nil ? (__bridge void*)window.nsWindow : nullptr;
}

void* CocoaWindow::GetContentView() const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    return window != nil ? (__bridge void*)window.contentView : nullptr;
}

void* CocoaWindow::GetObjCWindow() const noexcept {
    return _objcWindow;
}

Eigen::Vector2i CocoaWindow::GetSize() const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window == nil) {
        return Eigen::Vector2i{0, 0};
    }
    NSSize size = [window contentSize];
    return Eigen::Vector2i{ToInt(size.width), ToInt(size.height)};
}

Eigen::Vector2i CocoaWindow::GetPosition() const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window == nil) {
        return Eigen::Vector2i{0, 0};
    }
    NSPoint pos = [window contentPosition];
    return Eigen::Vector2i{ToInt(pos.x), ToInt(pos.y)};
}

float CocoaWindow::GetDpiScale() const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window == nil) {
        return 1.0f;
    }
    return static_cast<float>([window dpiScale]);
}

bool CocoaWindow::IsMinimized() const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    return window != nil && [window isMinimized];
}

bool CocoaWindow::IsFocused() const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    return window != nil && [window isFocused];
}

void CocoaWindow::SetSize(int width, int height) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setContentSize:NSMakeSize(static_cast<CGFloat>(width), static_cast<CGFloat>(height))];
    }
}

void CocoaWindow::SetPosition(int x, int y) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setContentPosition:NSMakePoint(static_cast<CGFloat>(x), static_cast<CGFloat>(y))];
    }
}

void CocoaWindow::SetTitle(std::string_view title) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setTitle:ToNSString(title)];
    }
}

void CocoaWindow::SetAlpha(float alpha) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setAlpha:static_cast<CGFloat>(std::clamp(alpha, 0.0f, 1.0f))];
    }
}

void CocoaWindow::SetOwner(Nullable<NativeWindow*> owner) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setOwnerWindow:ExtractCocoaOwner(owner)];
    }
}

void CocoaWindow::SetDecorated(bool value) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setDecorated:value];
    }
}

void CocoaWindow::SetShowInTaskbar(bool value) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setShowInTaskbar:value];
    }
}

void CocoaWindow::SetTopMost(bool value) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setTopMost:value];
    }
}

void CocoaWindow::SetFocusOnClick(bool value) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setFocusOnClick:value];
    }
}

void CocoaWindow::SetInputPassthrough(bool value) noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window != nil) {
        [window setInputPassthrough:value];
    }
}

Eigen::Vector2i CocoaWindow::ClientToScreen(Eigen::Vector2i pos) const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window == nil) {
        return pos;
    }
    NSPoint result = [window clientToScreen:NSMakePoint(static_cast<CGFloat>(pos.x()), static_cast<CGFloat>(pos.y()))];
    return Eigen::Vector2i{ToInt(result.x), ToInt(result.y)};
}

Eigen::Vector2i CocoaWindow::ScreenToClient(Eigen::Vector2i pos) const noexcept {
    RadrayCocoaWindow* window = CocoaWindowObjC(this);
    if (window == nil) {
        return pos;
    }
    NSPoint result = [window screenToClient:NSMakePoint(static_cast<CGFloat>(pos.x()), static_cast<CGFloat>(pos.y()))];
    return Eigen::Vector2i{ToInt(result.x), ToInt(result.y)};
}

sigslot::signal<int, int>& CocoaWindow::EventResized() noexcept { return _eventResized; }
sigslot::signal<int, int, MouseButton, Action>& CocoaWindow::EventTouch() noexcept { return _eventTouch; }
sigslot::signal<KeyCode, Action>& CocoaWindow::EventKeyboard() noexcept { return _eventKeyboard; }
sigslot::signal<int>& CocoaWindow::EventMouseWheel() noexcept { return _eventMouseWheel; }
sigslot::signal<std::string_view>& CocoaWindow::EventTextInput() noexcept { return _eventTextInput; }
sigslot::signal<bool>& CocoaWindow::EventFocused() noexcept { return _eventFocused; }
sigslot::signal<>& CocoaWindow::EventCloseRequested() noexcept { return _eventCloseRequested; }
sigslot::signal<int, int>& CocoaWindow::EventMoved() noexcept { return _eventMoved; }
sigslot::signal<>& CocoaWindow::EventMouseLeave() noexcept { return _eventMouseLeave; }

void CocoaWindow::GlobalInit() noexcept {
    RadrayCocoaWindowGlobalInit();
}

void CocoaWindow::GlobalShutdown() noexcept {
    RadrayCocoaWindowGlobalShutdown();
}

Nullable<unique_ptr<CocoaWindow>> CocoaWindow::Create(const CocoaWindowCreateDescriptor& desc) noexcept {
    auto result = make_unique<CocoaWindow>();
    RadrayCocoaWindowBridgeDelegate* delegate = [[RadrayCocoaWindowBridgeDelegate alloc] initWithWindow:result.get()];
    RadrayCocoaWindow* window = [[RadrayCocoaWindow alloc] initWithDescriptor:ToCocoaDescriptor(desc)];
    if (window == nil) {
        RADRAY_ERR_LOG("RadrayCocoaWindow create failed");
        return nullptr;
    }
    window.delegate = delegate;
    result->_objcWindow = RetainObjCObject(window);
    result->_objcDelegate = RetainObjCObject(delegate);
    return result;
}

KeyCode MapCocoaKeyCodeToKeyCode(unsigned short keyCode) noexcept {
    switch (keyCode) {
        case kVK_Space: return KeyCode::SPACE;
        case kVK_ANSI_Quote: return KeyCode::APOSTROPHE;
        case kVK_ANSI_Comma: return KeyCode::COMMA;
        case kVK_ANSI_Minus: return KeyCode::MINUS;
        case kVK_ANSI_Period: return KeyCode::PERIOD;
        case kVK_ANSI_Slash: return KeyCode::SLASH;
        case kVK_ANSI_0: return KeyCode::NUM0;
        case kVK_ANSI_1: return KeyCode::NUM1;
        case kVK_ANSI_2: return KeyCode::NUM2;
        case kVK_ANSI_3: return KeyCode::NUM3;
        case kVK_ANSI_4: return KeyCode::NUM4;
        case kVK_ANSI_5: return KeyCode::NUM5;
        case kVK_ANSI_6: return KeyCode::NUM6;
        case kVK_ANSI_7: return KeyCode::NUM7;
        case kVK_ANSI_8: return KeyCode::NUM8;
        case kVK_ANSI_9: return KeyCode::NUM9;
        case kVK_ANSI_Semicolon: return KeyCode::SEMICOLON;
        case kVK_ANSI_Equal: return KeyCode::EQUAL;
        case kVK_ANSI_A: return KeyCode::A;
        case kVK_ANSI_B: return KeyCode::B;
        case kVK_ANSI_C: return KeyCode::C;
        case kVK_ANSI_D: return KeyCode::D;
        case kVK_ANSI_E: return KeyCode::E;
        case kVK_ANSI_F: return KeyCode::F;
        case kVK_ANSI_G: return KeyCode::G;
        case kVK_ANSI_H: return KeyCode::H;
        case kVK_ANSI_I: return KeyCode::I;
        case kVK_ANSI_J: return KeyCode::J;
        case kVK_ANSI_K: return KeyCode::K;
        case kVK_ANSI_L: return KeyCode::L;
        case kVK_ANSI_M: return KeyCode::M;
        case kVK_ANSI_N: return KeyCode::N;
        case kVK_ANSI_O: return KeyCode::O;
        case kVK_ANSI_P: return KeyCode::P;
        case kVK_ANSI_Q: return KeyCode::Q;
        case kVK_ANSI_R: return KeyCode::R;
        case kVK_ANSI_S: return KeyCode::S;
        case kVK_ANSI_T: return KeyCode::T;
        case kVK_ANSI_U: return KeyCode::U;
        case kVK_ANSI_V: return KeyCode::V;
        case kVK_ANSI_W: return KeyCode::W;
        case kVK_ANSI_X: return KeyCode::X;
        case kVK_ANSI_Y: return KeyCode::Y;
        case kVK_ANSI_Z: return KeyCode::Z;
        case kVK_ANSI_LeftBracket: return KeyCode::LEFT_BRACKET;
        case kVK_ANSI_Backslash: return KeyCode::BACKSLASH;
        case kVK_ANSI_RightBracket: return KeyCode::RIGHT_BRACKET;
        case kVK_ANSI_Grave: return KeyCode::GRAVE_ACCENT;
        case kVK_Escape: return KeyCode::ESCAPE;
        case kVK_Return: return KeyCode::ENTER;
        case kVK_Tab: return KeyCode::TAB;
        case kVK_Delete: return KeyCode::BACKSPACE;
        case kVK_ForwardDelete: return KeyCode::DELETE;
        case kVK_RightArrow: return KeyCode::RIGHT;
        case kVK_LeftArrow: return KeyCode::LEFT;
        case kVK_DownArrow: return KeyCode::DOWN;
        case kVK_UpArrow: return KeyCode::UP;
        case kVK_PageUp: return KeyCode::PAGE_UP;
        case kVK_PageDown: return KeyCode::PAGE_DOWN;
        case kVK_Home: return KeyCode::HOME;
        case kVK_End: return KeyCode::END;
        case kVK_CapsLock: return KeyCode::CAPS_LOCK;
        case kVK_F1: return KeyCode::F1;
        case kVK_F2: return KeyCode::F2;
        case kVK_F3: return KeyCode::F3;
        case kVK_F4: return KeyCode::F4;
        case kVK_F5: return KeyCode::F5;
        case kVK_F6: return KeyCode::F6;
        case kVK_F7: return KeyCode::F7;
        case kVK_F8: return KeyCode::F8;
        case kVK_F9: return KeyCode::F9;
        case kVK_F10: return KeyCode::F10;
        case kVK_F11: return KeyCode::F11;
        case kVK_F12: return KeyCode::F12;
        case kVK_F13: return KeyCode::F13;
        case kVK_F14: return KeyCode::F14;
        case kVK_F15: return KeyCode::F15;
        case kVK_F16: return KeyCode::F16;
        case kVK_F17: return KeyCode::F17;
        case kVK_F18: return KeyCode::F18;
        case kVK_F19: return KeyCode::F19;
        case kVK_F20: return KeyCode::F20;
        case kVK_ANSI_Keypad0: return KeyCode::KP_0;
        case kVK_ANSI_Keypad1: return KeyCode::KP_1;
        case kVK_ANSI_Keypad2: return KeyCode::KP_2;
        case kVK_ANSI_Keypad3: return KeyCode::KP_3;
        case kVK_ANSI_Keypad4: return KeyCode::KP_4;
        case kVK_ANSI_Keypad5: return KeyCode::KP_5;
        case kVK_ANSI_Keypad6: return KeyCode::KP_6;
        case kVK_ANSI_Keypad7: return KeyCode::KP_7;
        case kVK_ANSI_Keypad8: return KeyCode::KP_8;
        case kVK_ANSI_Keypad9: return KeyCode::KP_9;
        case kVK_ANSI_KeypadDecimal: return KeyCode::KP_DECIMAL;
        case kVK_ANSI_KeypadDivide: return KeyCode::KP_DIVIDE;
        case kVK_ANSI_KeypadMultiply: return KeyCode::KP_MULTIPLY;
        case kVK_ANSI_KeypadMinus: return KeyCode::KP_SUBTRACT;
        case kVK_ANSI_KeypadPlus: return KeyCode::KP_ADD;
        case kVK_ANSI_KeypadEnter: return KeyCode::KP_ENTER;
        case kVK_ANSI_KeypadEquals: return KeyCode::KP_EQUAL;
        case kVK_Shift: return KeyCode::LEFT_SHIFT;
        case kVK_Control: return KeyCode::LEFT_CONTROL;
        case kVK_Option: return KeyCode::LEFT_ALT;
        case kVK_Command: return KeyCode::LEFT_SUPER;
        case kVK_RightShift: return KeyCode::RIGHT_SHIFT;
        case kVK_RightControl: return KeyCode::RIGHT_CONTROL;
        case kVK_RightOption: return KeyCode::RIGHT_ALT;
        case kVK_RightCommand: return KeyCode::RIGHT_SUPER;
        default: return KeyCode::UNKNOWN;
    }
}

}  // namespace radray

@implementation RadrayCocoaWindowBridgeDelegate

- (instancetype)initWithWindow:(radray::CocoaWindow*)window {
    self = [super init];
    if (self != nil) {
        _window = window;
    }
    return self;
}

- (void)cocoaWindow:(RadrayCocoaWindow*)window didResizeToContentSize:(NSSize)size {
    (void)window;
    if (self.window != nullptr) {
        self.window->EventResized()(radray::ToInt(size.width), radray::ToInt(size.height));
    }
}

- (void)cocoaWindow:(RadrayCocoaWindow*)window didMoveToContentPosition:(NSPoint)position {
    (void)window;
    if (self.window != nullptr) {
        self.window->EventMoved()(radray::ToInt(position.x), radray::ToInt(position.y));
    }
}

- (void)cocoaWindow:(RadrayCocoaWindow*)window didReceiveMouseButton:(RadrayCocoaMouseButton)button action:(RadrayCocoaWindowAction)action position:(NSPoint)position {
    (void)window;
    if (self.window != nullptr) {
        self.window->EventTouch()(radray::ToInt(position.x), radray::ToInt(position.y), radray::ToMouseButton(button), radray::ToAction(action));
    }
}

- (void)cocoaWindow:(RadrayCocoaWindow*)window didMoveMouseToPosition:(NSPoint)position button:(RadrayCocoaMouseButton)button {
    (void)window;
    if (self.window != nullptr) {
        self.window->EventTouch()(radray::ToInt(position.x), radray::ToInt(position.y), radray::ToMouseButton(button), radray::Action::REPEATED);
    }
}

- (void)cocoaWindow:(RadrayCocoaWindow*)window didScrollWithDeltaX:(CGFloat)deltaX deltaY:(CGFloat)deltaY {
    (void)window;
    (void)deltaX;
    if (self.window != nullptr) {
        self.window->EventMouseWheel()(radray::ToInt(deltaY));
    }
}

- (void)cocoaWindow:(RadrayCocoaWindow*)window didReceiveKeyCode:(unsigned short)keyCode action:(RadrayCocoaWindowAction)action {
    (void)window;
    if (self.window != nullptr) {
        self.window->EventKeyboard()(radray::MapCocoaKeyCodeToKeyCode(keyCode), radray::ToAction(action));
    }
}

- (void)cocoaWindow:(RadrayCocoaWindow*)window didReceiveTextInput:(NSString*)text {
    (void)window;
    if (self.window == nullptr) {
        return;
    }
    const char* utf8 = [text UTF8String];
    if (utf8 == nullptr) {
        return;
    }
    self.window->EventTextInput()(std::string_view{utf8});
}

- (void)cocoaWindow:(RadrayCocoaWindow*)window didChangeFocus:(BOOL)focused {
    (void)window;
    if (self.window != nullptr) {
        self.window->EventFocused()(focused);
    }
}

- (void)cocoaWindowDidRequestClose:(RadrayCocoaWindow*)window {
    (void)window;
    if (self.window != nullptr) {
        self.window->EventCloseRequested()();
    }
}

- (void)cocoaWindowDidMouseLeave:(RadrayCocoaWindow*)window {
    (void)window;
    if (self.window != nullptr) {
        self.window->EventMouseLeave()();
    }
}

@end

#endif
