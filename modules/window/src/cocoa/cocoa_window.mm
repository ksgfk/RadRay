#include "cocoa_window.h"

#include <radray/logger.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Carbon/Carbon.h>

namespace radray {

static KeyCode MapCocoaKeyToKeyCode(unsigned short keyCode) noexcept {
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

static MouseButton MapCocoaNSButtonToMouseButton(NSInteger buttonNumber) noexcept {
    switch (buttonNumber) {
        case 0: return MouseButton::BUTTON_LEFT;
        case 1: return MouseButton::BUTTON_RIGHT;
        case 2: return MouseButton::BUTTON_MIDDLE;
        case 3: return MouseButton::BUTTON_4;
        case 4: return MouseButton::BUTTON_5;
        default: return MouseButton::UNKNOWN;
    }
}

}  // namespace radray

@interface RadrayWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, assign) radray::CocoaWindow* owner;
@end

@implementation RadrayWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender {
    if (_owner) {
        _owner->_closeRequested = true;
    }
    return NO;
}

- (void)windowDidResize:(NSNotification*)notification {
    if (!_owner) return;
    NSView* view = _owner->_nsView;
    NSSize size = [view bounds].size;
    NSSize backing = [view convertSizeToBacking:size];
    _owner->_eventResized((int)backing.width, (int)backing.height);
}

- (void)windowWillStartLiveResize:(NSNotification*)notification {
    if (!_owner) return;
    NSView* view = _owner->_nsView;
    NSSize size = [view bounds].size;
    NSSize backing = [view convertSizeToBacking:size];
    _owner->_eventResizing((int)backing.width, (int)backing.height);
}

- (void)windowDidEndLiveResize:(NSNotification*)notification {
    if (!_owner) return;
    NSView* view = _owner->_nsView;
    NSSize size = [view bounds].size;
    NSSize backing = [view convertSizeToBacking:size];
    _owner->_eventResized((int)backing.width, (int)backing.height);
}

@end

@interface RadrayView : NSView
@property(nonatomic, assign) radray::CocoaWindow* owner;
@end

@implementation RadrayView

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self) {
        [self setWantsLayer:YES];
        self.layer = [CAMetalLayer layer];
    }
    return self;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    return YES;
}

- (void)mouseDown:(NSEvent*)event {
    [self handleMouseEvent:event button:radray::MouseButton::BUTTON_LEFT action:radray::Action::PRESSED];
}
- (void)mouseUp:(NSEvent*)event {
    [self handleMouseEvent:event button:radray::MouseButton::BUTTON_LEFT action:radray::Action::RELEASED];
}
- (void)rightMouseDown:(NSEvent*)event {
    [self handleMouseEvent:event button:radray::MouseButton::BUTTON_RIGHT action:radray::Action::PRESSED];
}
- (void)rightMouseUp:(NSEvent*)event {
    [self handleMouseEvent:event button:radray::MouseButton::BUTTON_RIGHT action:radray::Action::RELEASED];
}
- (void)otherMouseDown:(NSEvent*)event {
    radray::MouseButton btn = radray::MapCocoaNSButtonToMouseButton([event buttonNumber]);
    [self handleMouseEvent:event button:btn action:radray::Action::PRESSED];
}
- (void)otherMouseUp:(NSEvent*)event {
    radray::MouseButton btn = radray::MapCocoaNSButtonToMouseButton([event buttonNumber]);
    [self handleMouseEvent:event button:btn action:radray::Action::RELEASED];
}
- (void)mouseMoved:(NSEvent*)event {
    [self handleMouseEvent:event button:radray::MouseButton::UNKNOWN action:radray::Action::REPEATED];
}
- (void)mouseDragged:(NSEvent*)event {
    [self handleMouseEvent:event button:radray::MouseButton::BUTTON_LEFT action:radray::Action::REPEATED];
}
- (void)rightMouseDragged:(NSEvent*)event {
    [self handleMouseEvent:event button:radray::MouseButton::BUTTON_RIGHT action:radray::Action::REPEATED];
}
- (void)otherMouseDragged:(NSEvent*)event {
    radray::MouseButton btn = radray::MapCocoaNSButtonToMouseButton([event buttonNumber]);
    [self handleMouseEvent:event button:btn action:radray::Action::REPEATED];
}

- (void)handleMouseEvent:(NSEvent*)event button:(radray::MouseButton)btn action:(radray::Action)action {
    if (!_owner) return;
    NSPoint loc = [self convertPoint:[event locationInWindow] fromView:nil];
    NSSize backing = [self convertSizeToBacking:NSMakeSize(1, 1)];
    int x = (int)(loc.x * backing.width);
    int y = (int)((self.bounds.size.height - loc.y) * backing.height);
    _owner->_eventTouch(x, y, btn, action);
}

- (void)scrollWheel:(NSEvent*)event {
    if (!_owner) return;
    _owner->_eventMouseWheel((int)[event scrollingDeltaY]);
}

- (void)keyDown:(NSEvent*)event {
    if (!_owner) return;
    radray::KeyCode code = radray::MapCocoaKeyToKeyCode([event keyCode]);
    radray::Action action = [event isARepeat] ? radray::Action::REPEATED : radray::Action::PRESSED;
    _owner->_eventKeyboard(code, action);
}

- (void)keyUp:(NSEvent*)event {
    if (!_owner) return;
    _owner->_eventKeyboard(radray::MapCocoaKeyToKeyCode([event keyCode]), radray::Action::RELEASED);
}

- (void)flagsChanged:(NSEvent*)event {
    if (!_owner) return;
    radray::KeyCode code = radray::MapCocoaKeyToKeyCode([event keyCode]);
    NSEventModifierFlags flags = [event modifierFlags];
    radray::Action action;
    switch ([event keyCode]) {
        case kVK_Shift:
        case kVK_RightShift:
            action = (flags & NSEventModifierFlagShift) ? radray::Action::PRESSED : radray::Action::RELEASED;
            break;
        case kVK_Control:
        case kVK_RightControl:
            action = (flags & NSEventModifierFlagControl) ? radray::Action::PRESSED : radray::Action::RELEASED;
            break;
        case kVK_Option:
        case kVK_RightOption:
            action = (flags & NSEventModifierFlagOption) ? radray::Action::PRESSED : radray::Action::RELEASED;
            break;
        case kVK_Command:
        case kVK_RightCommand:
            action = (flags & NSEventModifierFlagCommand) ? radray::Action::PRESSED : radray::Action::RELEASED;
            break;
        case kVK_CapsLock:
            action = (flags & NSEventModifierFlagCapsLock) ? radray::Action::PRESSED : radray::Action::RELEASED;
            break;
        default: return;
    }
    _owner->_eventKeyboard(code, action);
}

@end

namespace radray {

static bool g_nsAppInitialized = false;

static void EnsureNSAppInitialized() {
    if (g_nsAppInitialized) return;
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSMenu* menuBar = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [menuBar addItem:appMenuItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    [appMenu addItemWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
    [appMenuItem setSubmenu:appMenu];
    [NSApp setMainMenu:menuBar];

    [NSApp finishLaunching];
    g_nsAppInitialized = true;
}

Nullable<unique_ptr<NativeWindow>> CreateCocoaWindow(const CocoaWindowCreateDescriptor& desc) noexcept {
    EnsureNSAppInitialized();

    NSWindowStyleMask styleMask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable;
    if (desc.Resizable) {
        styleMask |= NSWindowStyleMaskResizable;
    }

    NSRect contentRect = NSMakeRect(
        desc.X >= 0 ? desc.X : 100,
        desc.Y >= 0 ? desc.Y : 100,
        desc.Width, desc.Height);

    NSWindow* window = [[NSWindow alloc]
        initWithContentRect:contentRect
                  styleMask:styleMask
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (!window) {
        RADRAY_ERR_LOG("Failed to create NSWindow");
        return nullptr;
    }

    [window setTitle:[NSString stringWithUTF8String:std::string(desc.Title).c_str()]];
    [window setAcceptsMouseMovedEvents:YES];

    RadrayView* view = [[RadrayView alloc] initWithFrame:[window contentLayoutRect]];
    [window setContentView:view];

    auto win = std::make_unique<CocoaWindow>();

    RadrayWindowDelegate* delegate = [[RadrayWindowDelegate alloc] init];
    delegate.owner = win.get();
    view.owner = win.get();
    [window setDelegate:delegate];

    win->_nsWindow = window;
    win->_nsView = view;
    win->_windowDelegate = delegate;

    if (desc.StartMaximized) {
        [window zoom:nil];
    }
    if (desc.Fullscreen) {
        [window toggleFullScreen:nil];
    }

    [window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    return win;
}

CocoaWindow::CocoaWindow() noexcept {}

CocoaWindow::~CocoaWindow() noexcept {
    this->DestroyImpl();
}

bool CocoaWindow::IsValid() const noexcept {
    return _nsWindow != nullptr;
}

void CocoaWindow::Destroy() noexcept {
    this->DestroyImpl();
}

void CocoaWindow::DestroyImpl() noexcept {
    if (_nsWindow) {
        RadrayWindowDelegate* delegate = _windowDelegate;
        delegate.owner = nullptr;
        RadrayView* view = (RadrayView*)_nsView;
        view.owner = nullptr;
        [_nsWindow setDelegate:nil];
        [_nsWindow close];
        _windowDelegate = nullptr;
        _nsView = nullptr;
        _nsWindow = nullptr;
    }
}

void CocoaWindow::DispatchEvents() noexcept {
    @autoreleasepool {
        while (true) {
            NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:nil
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (!event) break;
            [NSApp sendEvent:event];
        }
    }
}

bool CocoaWindow::ShouldClose() const noexcept {
    return _closeRequested;
}

WindowNativeHandler CocoaWindow::GetNativeHandler() const noexcept {
    return WindowNativeHandler{WindowHandlerTag::NS_VIEW, (__bridge void*)_nsView};
}

WindowVec2i CocoaWindow::GetSize() const noexcept {
    if (!_nsView) return WindowVec2i{0, 0};
    NSSize size = [_nsView bounds].size;
    NSSize backing = [_nsView convertSizeToBacking:size];
    return WindowVec2i{(int32_t)backing.width, (int32_t)backing.height};
}

bool CocoaWindow::IsMinimized() const noexcept {
    if (!_nsWindow) return false;
    return [_nsWindow isMiniaturized];
}

void CocoaWindow::SetSize(int width, int height) noexcept {
    if (!_nsWindow || width <= 0 || height <= 0) return;
    NSSize backing = [_nsView convertSizeToBacking:NSMakeSize(1, 1)];
    CGFloat w = width / backing.width;
    CGFloat h = height / backing.height;
    [_nsWindow setContentSize:NSMakeSize(w, h)];
}

sigslot::signal<int, int>& CocoaWindow::EventResized() noexcept { return _eventResized; }
sigslot::signal<int, int>& CocoaWindow::EventResizing() noexcept { return _eventResizing; }
sigslot::signal<int, int, MouseButton, Action>& CocoaWindow::EventTouch() noexcept { return _eventTouch; }
sigslot::signal<KeyCode, Action>& CocoaWindow::EventKeyboard() noexcept { return _eventKeyboard; }
sigslot::signal<int>& CocoaWindow::EventMouseWheel() noexcept { return _eventMouseWheel; }

}  // namespace radray
