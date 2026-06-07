#import <radray/window/cocoa/cocoa_window_objc.h>

#import <Carbon/Carbon.h>
#import <CoreGraphics/CGEventTypes.h>

@class RadrayCocoaNativeWindow;
@class RadrayCocoaContentView;

@interface RadrayCocoaWindow () <NSWindowDelegate>
@property(nonatomic, strong, nullable) RadrayCocoaNativeWindow* nativeWindow;
@property(nonatomic, strong, nullable) RadrayCocoaContentView* nativeContentView;
@property(nonatomic, weak, nullable) RadrayCocoaWindow* ownerWindow;
@property(nonatomic, assign) BOOL closeRequested;
@property(nonatomic, assign) BOOL resizable;
@property(nonatomic, assign) BOOL decorated;
@property(nonatomic, assign) BOOL showInTaskbar;
@property(nonatomic, assign) BOOL topMost;
@property(nonatomic, assign) BOOL focusOnClick;
@property(nonatomic, assign) BOOL inputPassthrough;
@property(nonatomic, assign) BOOL pendingFullscreen;
@end

@interface RadrayCocoaNativeWindow : NSWindow
@property(nonatomic, weak, nullable) RadrayCocoaWindow* radrayWindow;
@end

@interface RadrayCocoaContentView : NSView <NSTextInputClient>
@property(nonatomic, weak, nullable) RadrayCocoaWindow* radrayWindow;
@property(nonatomic, strong) NSMutableAttributedString* markedText;
@end

static NSUInteger g_radrayCocoaGlobalInitCount = 0;

static CGFloat RadrayCocoaClamp(CGFloat value, CGFloat minValue, CGFloat maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

static CGFloat RadrayCocoaGlobalTopY(void) {
    CGFloat topY = 0.0;
    BOOL hasScreen = NO;
    for (NSScreen* screen in [NSScreen screens]) {
        topY = hasScreen ? MAX(topY, NSMaxY(screen.frame)) : NSMaxY(screen.frame);
        hasScreen = YES;
    }
    return topY;
}

static NSPoint RadrayScreenPointFromCocoaScreenPoint(NSPoint point) {
    return NSMakePoint(point.x, RadrayCocoaGlobalTopY() - point.y);
}

static NSPoint RadrayCocoaScreenPointFromScreenPoint(NSPoint point) {
    return NSMakePoint(point.x, RadrayCocoaGlobalTopY() - point.y);
}

static NSWindowStyleMask RadrayCocoaBuildStyleMask(BOOL decorated, BOOL resizable) {
    NSWindowStyleMask style = NSWindowStyleMaskMiniaturizable;
    if (decorated) {
        style |= NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
    } else {
        style |= NSWindowStyleMaskBorderless;
    }
    if (resizable) {
        style |= NSWindowStyleMaskResizable;
    }
    return style;
}

static RadrayCocoaMouseButton RadrayCocoaMouseButtonFromEvent(NSEvent* event) {
    NSInteger buttonNumber = event.buttonNumber;
    if (buttonNumber < 0 || buttonNumber >= 8) {
        return RadrayCocoaMouseButtonUnknown;
    }
    return (RadrayCocoaMouseButton)(buttonNumber + 1);
}

typedef NS_ENUM(NSUInteger, RadrayCocoaExtendedMouseButtonIndex) {
    RadrayCocoaExtendedMouseButton4Index = 3,
    RadrayCocoaExtendedMouseButton5Index = 4,
    RadrayCocoaExtendedMouseButton6Index = 5,
    RadrayCocoaExtendedMouseButton7Index = 6,
    RadrayCocoaExtendedMouseButton8Index = 7,
};

static NSUInteger RadrayCocoaPressedMouseButtonMask(NSUInteger buttonIndex) {
    return 1u << buttonIndex;
}

static RadrayCocoaMouseButton RadrayCocoaMouseButtonFromPressedButtons(NSUInteger buttons) {
    if ((buttons & RadrayCocoaPressedMouseButtonMask(kCGMouseButtonLeft)) != 0) {
        return RadrayCocoaMouseButtonLeft;
    }
    if ((buttons & RadrayCocoaPressedMouseButtonMask(kCGMouseButtonRight)) != 0) {
        return RadrayCocoaMouseButtonRight;
    }
    if ((buttons & RadrayCocoaPressedMouseButtonMask(kCGMouseButtonCenter)) != 0) {
        return RadrayCocoaMouseButtonMiddle;
    }
    if ((buttons & RadrayCocoaPressedMouseButtonMask(RadrayCocoaExtendedMouseButton4Index)) != 0) {
        return RadrayCocoaMouseButton4;
    }
    if ((buttons & RadrayCocoaPressedMouseButtonMask(RadrayCocoaExtendedMouseButton5Index)) != 0) {
        return RadrayCocoaMouseButton5;
    }
    if ((buttons & RadrayCocoaPressedMouseButtonMask(RadrayCocoaExtendedMouseButton6Index)) != 0) {
        return RadrayCocoaMouseButton6;
    }
    if ((buttons & RadrayCocoaPressedMouseButtonMask(RadrayCocoaExtendedMouseButton7Index)) != 0) {
        return RadrayCocoaMouseButton7;
    }
    if ((buttons & RadrayCocoaPressedMouseButtonMask(RadrayCocoaExtendedMouseButton8Index)) != 0) {
        return RadrayCocoaMouseButton8;
    }
    return RadrayCocoaMouseButtonUnknown;
}

static NSEventModifierFlags RadrayCocoaModifierFlagForKeyCode(unsigned short keyCode) {
    switch (keyCode) {
        case kVK_RightCommand:
        case kVK_Command:
            return NSEventModifierFlagCommand;
        case kVK_Shift:
        case kVK_RightShift:
            return NSEventModifierFlagShift;
        case kVK_CapsLock:
            return NSEventModifierFlagCapsLock;
        case kVK_Option:
        case kVK_RightOption:
            return NSEventModifierFlagOption;
        case kVK_Control:
        case kVK_RightControl:
            return NSEventModifierFlagControl;
        case kVK_Function:
            return NSEventModifierFlagFunction;
        default:
            return 0;
    }
}

static BOOL RadrayCocoaIsModifierKeyCode(unsigned short keyCode) {
    return RadrayCocoaModifierFlagForKeyCode(keyCode) != 0;
}

static NSRect RadrayCocoaInitialContentRect(RadrayCocoaWindowDescriptor descriptor) {
    CGFloat width = MAX(descriptor.width, 1.0);
    CGFloat height = MAX(descriptor.height, 1.0);
    if (descriptor.x < 0.0 || descriptor.y < 0.0) {
        return NSMakeRect(0.0, 0.0, width, height);
    }

    NSPoint contentTopLeft = RadrayCocoaScreenPointFromScreenPoint(NSMakePoint(descriptor.x, descriptor.y));
    return NSMakeRect(contentTopLeft.x, contentTopLeft.y - height, width, height);
}

RadrayCocoaWindowDescriptor RadrayCocoaWindowDescriptorMakeDefault(void) {
    RadrayCocoaWindowDescriptor descriptor = {0};
    descriptor.title = @"";
    descriptor.startVisible = YES;
    descriptor.decorated = YES;
    descriptor.showInTaskbar = YES;
    descriptor.activateOnShow = YES;
    descriptor.focusOnClick = YES;
    return descriptor;
}

void RadrayCocoaWindowGlobalInit(void) {
    if (g_radrayCocoaGlobalInitCount++ > 0) {
        return;
    }

    [NSApplication sharedApplication];
    [NSApp finishLaunching];
}

void RadrayCocoaWindowGlobalShutdown(void) {
    if (g_radrayCocoaGlobalInitCount > 0) {
        --g_radrayCocoaGlobalInitCount;
    }
}

@implementation RadrayCocoaNativeWindow

- (BOOL)canBecomeKeyWindow {
    return YES;
}

- (BOOL)canBecomeMainWindow {
    return YES;
}

@end

@implementation RadrayCocoaContentView

- (instancetype)initWithFrame:(NSRect)frameRect {
    self = [super initWithFrame:frameRect];
    if (self != nil) {
        _markedText = [[NSMutableAttributedString alloc] init];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (BOOL)canBecomeKeyView {
    return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return self.radrayWindow == nil || self.radrayWindow.focusOnClick;
}

- (void)viewDidMoveToWindow {
    [super viewDidMoveToWindow];
    self.window.acceptsMouseMovedEvents = YES;
    [self.window makeFirstResponder:self];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    for (NSTrackingArea* trackingArea in self.trackingAreas) {
        [self removeTrackingArea:trackingArea];
    }

    NSTrackingAreaOptions options =
        NSTrackingMouseEnteredAndExited |
        NSTrackingActiveAlways |
        NSTrackingInVisibleRect;
    NSTrackingArea* trackingArea = [[NSTrackingArea alloc] initWithRect:NSZeroRect options:options owner:self userInfo:nil];
    [self addTrackingArea:trackingArea];
}

- (NSPoint)radrayPointFromEvent:(NSEvent*)event {
    return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)emitMouseButtonEvent:(NSEvent*)event action:(RadrayCocoaWindowAction)action {
    RadrayCocoaWindow* window = self.radrayWindow;
    id<RadrayCocoaWindowDelegate> delegate = window.delegate;
    SEL selector = @selector(cocoaWindow:didReceiveMouseButton:action:position:);
    if (window == nil || ![delegate respondsToSelector:selector]) {
        return;
    }

    [delegate cocoaWindow:window
    didReceiveMouseButton:RadrayCocoaMouseButtonFromEvent(event)
                   action:action
                 position:[self radrayPointFromEvent:event]];
}

- (void)emitMouseMoveEvent:(NSEvent*)event {
    RadrayCocoaWindow* window = self.radrayWindow;
    id<RadrayCocoaWindowDelegate> delegate = window.delegate;
    SEL selector = @selector(cocoaWindow:didMoveMouseToPosition:button:);
    if (window == nil || ![delegate respondsToSelector:selector]) {
        return;
    }

    [delegate cocoaWindow:window
    didMoveMouseToPosition:[self radrayPointFromEvent:event]
                    button:RadrayCocoaMouseButtonFromPressedButtons((NSUInteger)[NSEvent pressedMouseButtons])];
}

- (void)mouseDown:(NSEvent*)event {
    if (self.radrayWindow.focusOnClick) {
        [self.window makeFirstResponder:self];
    }
    [self emitMouseButtonEvent:event action:RadrayCocoaWindowActionPressed];
}

- (void)mouseUp:(NSEvent*)event {
    [self emitMouseButtonEvent:event action:RadrayCocoaWindowActionReleased];
}

- (void)rightMouseDown:(NSEvent*)event {
    if (self.radrayWindow.focusOnClick) {
        [self.window makeFirstResponder:self];
    }
    [self emitMouseButtonEvent:event action:RadrayCocoaWindowActionPressed];
}

- (void)rightMouseUp:(NSEvent*)event {
    [self emitMouseButtonEvent:event action:RadrayCocoaWindowActionReleased];
}

- (void)otherMouseDown:(NSEvent*)event {
    if (self.radrayWindow.focusOnClick) {
        [self.window makeFirstResponder:self];
    }
    [self emitMouseButtonEvent:event action:RadrayCocoaWindowActionPressed];
}

- (void)otherMouseUp:(NSEvent*)event {
    [self emitMouseButtonEvent:event action:RadrayCocoaWindowActionReleased];
}

- (void)mouseMoved:(NSEvent*)event {
    [self emitMouseMoveEvent:event];
}

- (void)mouseDragged:(NSEvent*)event {
    [self emitMouseMoveEvent:event];
}

- (void)rightMouseDragged:(NSEvent*)event {
    [self emitMouseMoveEvent:event];
}

- (void)otherMouseDragged:(NSEvent*)event {
    [self emitMouseMoveEvent:event];
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    RadrayCocoaWindow* window = self.radrayWindow;
    id<RadrayCocoaWindowDelegate> delegate = window.delegate;
    SEL selector = @selector(cocoaWindowDidMouseLeave:);
    if (window != nil && [delegate respondsToSelector:selector]) {
        [delegate cocoaWindowDidMouseLeave:window];
    }
}

- (void)scrollWheel:(NSEvent*)event {
    RadrayCocoaWindow* window = self.radrayWindow;
    id<RadrayCocoaWindowDelegate> delegate = window.delegate;
    SEL selector = @selector(cocoaWindow:didScrollWithDeltaX:deltaY:);
    if (window != nil && [delegate respondsToSelector:selector]) {
        [delegate cocoaWindow:window didScrollWithDeltaX:event.scrollingDeltaX deltaY:event.scrollingDeltaY];
    }
}

- (void)keyDown:(NSEvent*)event {
    RadrayCocoaWindow* window = self.radrayWindow;
    id<RadrayCocoaWindowDelegate> delegate = window.delegate;
    SEL selector = @selector(cocoaWindow:didReceiveKeyCode:action:);
    if (window != nil && [delegate respondsToSelector:selector]) {
        [delegate cocoaWindow:window
            didReceiveKeyCode:event.keyCode
                       action:[event isARepeat] ? RadrayCocoaWindowActionRepeated : RadrayCocoaWindowActionPressed];
    }

    [self interpretKeyEvents:@[event]];
}

- (void)keyUp:(NSEvent*)event {
    RadrayCocoaWindow* window = self.radrayWindow;
    id<RadrayCocoaWindowDelegate> delegate = window.delegate;
    SEL selector = @selector(cocoaWindow:didReceiveKeyCode:action:);
    if (window != nil && [delegate respondsToSelector:selector]) {
        [delegate cocoaWindow:window didReceiveKeyCode:event.keyCode action:RadrayCocoaWindowActionReleased];
    }
}

- (void)flagsChanged:(NSEvent*)event {
    if (!RadrayCocoaIsModifierKeyCode(event.keyCode)) {
        [super flagsChanged:event];
        return;
    }

    RadrayCocoaWindowAction action =
        ((event.modifierFlags & RadrayCocoaModifierFlagForKeyCode(event.keyCode)) != 0)
            ? RadrayCocoaWindowActionPressed
            : RadrayCocoaWindowActionReleased;

    RadrayCocoaWindow* window = self.radrayWindow;
    id<RadrayCocoaWindowDelegate> delegate = window.delegate;
    SEL selector = @selector(cocoaWindow:didReceiveKeyCode:action:);
    if (window != nil && [delegate respondsToSelector:selector]) {
        [delegate cocoaWindow:window didReceiveKeyCode:event.keyCode action:action];
    }
}

- (void)insertText:(id)string replacementRange:(NSRange)replacementRange {
    (void)replacementRange;
    NSString* text = [string isKindOfClass:[NSAttributedString class]]
                         ? [(NSAttributedString*)string string]
                         : (NSString*)string;
    if (text.length == 0) {
        return;
    }

    [self unmarkText];

    RadrayCocoaWindow* window = self.radrayWindow;
    id<RadrayCocoaWindowDelegate> delegate = window.delegate;
    SEL selector = @selector(cocoaWindow:didReceiveTextInput:);
    if (window != nil && [delegate respondsToSelector:selector]) {
        [delegate cocoaWindow:window didReceiveTextInput:text];
    }
}

- (void)doCommandBySelector:(SEL)selector {
    (void)selector;
}

- (void)setMarkedText:(id)string selectedRange:(NSRange)selectedRange replacementRange:(NSRange)replacementRange {
    (void)selectedRange;
    (void)replacementRange;
    NSString* text = [string isKindOfClass:[NSAttributedString class]]
                         ? [(NSAttributedString*)string string]
                         : (NSString*)string;
    self.markedText = [[NSMutableAttributedString alloc] initWithString:text ?: @""];
}

- (void)unmarkText {
    [self.markedText setAttributedString:[[NSAttributedString alloc] initWithString:@""]];
}

- (NSRange)selectedRange {
    return NSMakeRange(NSNotFound, 0);
}

- (NSRange)markedRange {
    if (self.markedText.length == 0) {
        return NSMakeRange(NSNotFound, 0);
    }
    return NSMakeRange(0, self.markedText.length);
}

- (BOOL)hasMarkedText {
    return self.markedText.length > 0;
}

- (nullable NSAttributedString*)attributedSubstringForProposedRange:(NSRange)range actualRange:(nullable NSRangePointer)actualRange {
    if (actualRange != NULL) {
        *actualRange = NSMakeRange(NSNotFound, 0);
    }
    (void)range;
    return nil;
}

- (NSArray<NSAttributedStringKey>*)validAttributesForMarkedText {
    return @[];
}

- (NSRect)firstRectForCharacterRange:(NSRange)range actualRange:(nullable NSRangePointer)actualRange {
    if (actualRange != NULL) {
        *actualRange = NSMakeRange(NSNotFound, 0);
    }
    (void)range;
    NSPoint windowPoint = [self convertPoint:NSZeroPoint toView:nil];
    NSPoint screenPoint = [self.window convertPointToScreen:windowPoint];
    return NSMakeRect(screenPoint.x, screenPoint.y, 0.0, 0.0);
}

- (NSUInteger)characterIndexForPoint:(NSPoint)point {
    (void)point;
    return 0;
}

@end

@implementation RadrayCocoaWindow

- (nullable instancetype)initWithDescriptor:(RadrayCocoaWindowDescriptor)descriptor {
    self = [super init];
    if (self == nil) {
        return nil;
    }

    RadrayCocoaWindowGlobalInit();

    _resizable = descriptor.resizable;
    _decorated = descriptor.decorated;
    _showInTaskbar = descriptor.showInTaskbar;
    _topMost = descriptor.topMost;
    _focusOnClick = descriptor.focusOnClick;
    _inputPassthrough = descriptor.inputPassthrough;
    _pendingFullscreen = descriptor.fullscreen;

    NSRect contentRect = RadrayCocoaInitialContentRect(descriptor);
    NSWindowStyleMask style = RadrayCocoaBuildStyleMask(_decorated, _resizable);
    RadrayCocoaNativeWindow* window = [[RadrayCocoaNativeWindow alloc]
        initWithContentRect:contentRect
                  styleMask:style
                    backing:NSBackingStoreBuffered
                      defer:NO];
    if (window == nil) {
        return nil;
    }

    RadrayCocoaContentView* contentView = [[RadrayCocoaContentView alloc] initWithFrame:NSMakeRect(0.0, 0.0, contentRect.size.width, contentRect.size.height)];
    contentView.radrayWindow = self;
    window.radrayWindow = self;
    window.delegate = self;
    window.releasedWhenClosed = NO;
    window.acceptsMouseMovedEvents = YES;
    window.title = descriptor.title ?: @"";
    window.contentView = contentView;
    [window makeFirstResponder:contentView];

    _nativeWindow = window;
    _nativeContentView = contentView;

    if (descriptor.x < 0.0 || descriptor.y < 0.0) {
        [window center];
    }

    [self setOwnerWindow:descriptor.ownerWindow];
    [self setShowInTaskbar:_showInTaskbar];
    [self setTopMost:_topMost];
    [self setInputPassthrough:_inputPassthrough];

    if (descriptor.startMaximized) {
        [window zoom:nil];
    }

    if (descriptor.startVisible) {
        [self showWithMode:descriptor.activateOnShow ? RadrayCocoaWindowShowModeDefault : RadrayCocoaWindowShowModeNoActivate];
    }

    return self;
}

- (NSWindow*)nsWindow {
    return self.nativeWindow;
}

- (NSView*)contentView {
    return self.nativeContentView;
}

- (BOOL)isValid {
    return self.nativeWindow != nil;
}

- (void)destroy {
    RadrayCocoaNativeWindow* window = self.nativeWindow;
    if (window == nil) {
        return;
    }

    [self.ownerWindow.nsWindow removeChildWindow:window];
    window.delegate = nil;
    window.radrayWindow = nil;
    self.nativeContentView.radrayWindow = nil;
    [window orderOut:nil];
    [window close];
    self.nativeContentView = nil;
    self.nativeWindow = nil;
    self.ownerWindow = nil;
}

- (void)show {
    [self showWithMode:RadrayCocoaWindowShowModeDefault];
}

- (void)showWithMode:(RadrayCocoaWindowShowMode)mode {
    RadrayCocoaNativeWindow* window = self.nativeWindow;
    if (window == nil) {
        return;
    }

    if (mode == RadrayCocoaWindowShowModeNoActivate) {
        [window orderFront:nil];
    } else {
        [NSApp activateIgnoringOtherApps:YES];
        [window makeKeyAndOrderFront:nil];
        [window makeFirstResponder:self.nativeContentView];
    }

    if (self.pendingFullscreen) {
        self.pendingFullscreen = NO;
        [window toggleFullScreen:nil];
    }
}

- (void)focus {
    RadrayCocoaNativeWindow* window = self.nativeWindow;
    if (window == nil) {
        return;
    }

    [NSApp activateIgnoringOtherApps:YES];
    [window makeKeyAndOrderFront:nil];
    [window makeFirstResponder:self.nativeContentView];
}

- (BOOL)shouldClose {
    return self.closeRequested;
}

- (NSSize)contentSize {
    RadrayCocoaContentView* contentView = self.nativeContentView;
    return contentView != nil ? contentView.bounds.size : NSZeroSize;
}

- (NSPoint)contentPosition {
    return [self clientToScreen:NSZeroPoint];
}

- (CGFloat)dpiScale {
    RadrayCocoaNativeWindow* window = self.nativeWindow;
    if (window == nil || window.screen == nil) {
        return 1.0;
    }
    return window.screen.backingScaleFactor;
}

- (BOOL)isMinimized {
    return self.nativeWindow.miniaturized;
}

- (BOOL)isFocused {
    return self.nativeWindow.keyWindow;
}

- (void)setContentSize:(NSSize)size {
    RadrayCocoaNativeWindow* window = self.nativeWindow;
    if (window == nil || size.width <= 0.0 || size.height <= 0.0) {
        return;
    }

    [window setContentSize:size];
}

- (void)setContentPosition:(NSPoint)position {
    RadrayCocoaNativeWindow* window = self.nativeWindow;
    RadrayCocoaContentView* contentView = self.nativeContentView;
    if (window == nil || contentView == nil) {
        return;
    }

    NSPoint currentContentTopLeft = [window convertPointToScreen:[contentView convertPoint:NSZeroPoint toView:nil]];
    NSRect frame = window.frame;
    NSPoint currentWindowTopLeft = NSMakePoint(frame.origin.x, NSMaxY(frame));
    NSPoint offset = NSMakePoint(currentWindowTopLeft.x - currentContentTopLeft.x, currentWindowTopLeft.y - currentContentTopLeft.y);
    NSPoint desiredContentTopLeft = RadrayCocoaScreenPointFromScreenPoint(position);
    NSPoint desiredWindowTopLeft = NSMakePoint(desiredContentTopLeft.x + offset.x, desiredContentTopLeft.y + offset.y);
    [window setFrameTopLeftPoint:desiredWindowTopLeft];
}

- (void)setTitle:(NSString*)title {
    self.nativeWindow.title = title ?: @"";
}

- (void)setAlpha:(CGFloat)alpha {
    self.nativeWindow.alphaValue = RadrayCocoaClamp(alpha, 0.0, 1.0);
}

- (void)setOwnerWindow:(nullable RadrayCocoaWindow*)ownerWindow {
    RadrayCocoaNativeWindow* window = self.nativeWindow;
    if (window == nil || ownerWindow == self) {
        return;
    }

    [self.ownerWindow.nsWindow removeChildWindow:window];
    self.ownerWindow = ownerWindow;
    if (ownerWindow.nsWindow != nil) {
        [ownerWindow.nsWindow addChildWindow:window ordered:NSWindowAbove];
    }
}

- (void)setDecorated:(BOOL)value {
    if (self.decorated == value) {
        return;
    }
    self.decorated = value;
    self.nativeWindow.styleMask = RadrayCocoaBuildStyleMask(self.decorated, self.resizable);
}

- (void)setShowInTaskbar:(BOOL)value {
    self.showInTaskbar = value;
    NSWindowCollectionBehavior behavior = self.nativeWindow.collectionBehavior;
    if (value) {
        behavior &= ~(NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle);
    } else {
        behavior |= NSWindowCollectionBehaviorTransient | NSWindowCollectionBehaviorIgnoresCycle;
    }
    self.nativeWindow.collectionBehavior = behavior;
}

- (void)setTopMost:(BOOL)value {
    self.topMost = value;
    self.nativeWindow.level = value ? NSFloatingWindowLevel : NSNormalWindowLevel;
}

- (void)setFocusOnClick:(BOOL)value {
    self.focusOnClick = value;
}

- (void)setInputPassthrough:(BOOL)value {
    self.inputPassthrough = value;
    self.nativeWindow.ignoresMouseEvents = value;
}

- (NSPoint)clientToScreen:(NSPoint)point {
    RadrayCocoaNativeWindow* window = self.nativeWindow;
    RadrayCocoaContentView* contentView = self.nativeContentView;
    if (window == nil || contentView == nil) {
        return point;
    }

    NSPoint windowPoint = [contentView convertPoint:point toView:nil];
    NSPoint screenPoint = [window convertPointToScreen:windowPoint];
    return RadrayScreenPointFromCocoaScreenPoint(screenPoint);
}

- (NSPoint)screenToClient:(NSPoint)point {
    RadrayCocoaNativeWindow* window = self.nativeWindow;
    RadrayCocoaContentView* contentView = self.nativeContentView;
    if (window == nil || contentView == nil) {
        return point;
    }

    NSPoint screenPoint = RadrayCocoaScreenPointFromScreenPoint(point);
    NSPoint windowPoint = [window convertPointFromScreen:screenPoint];
    return [contentView convertPoint:windowPoint fromView:nil];
}

- (void)windowDidResize:(NSNotification*)notification {
    (void)notification;
    id<RadrayCocoaWindowDelegate> delegate = self.delegate;
    SEL selector = @selector(cocoaWindow:didResizeToContentSize:);
    if ([delegate respondsToSelector:selector]) {
        [delegate cocoaWindow:self didResizeToContentSize:[self contentSize]];
    }
}

- (void)windowDidMove:(NSNotification*)notification {
    (void)notification;
    id<RadrayCocoaWindowDelegate> delegate = self.delegate;
    SEL selector = @selector(cocoaWindow:didMoveToContentPosition:);
    if ([delegate respondsToSelector:selector]) {
        [delegate cocoaWindow:self didMoveToContentPosition:[self contentPosition]];
    }
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    (void)notification;
    id<RadrayCocoaWindowDelegate> delegate = self.delegate;
    SEL selector = @selector(cocoaWindow:didChangeFocus:);
    if ([delegate respondsToSelector:selector]) {
        [delegate cocoaWindow:self didChangeFocus:YES];
    }
}

- (void)windowDidResignKey:(NSNotification*)notification {
    (void)notification;
    id<RadrayCocoaWindowDelegate> delegate = self.delegate;
    SEL selector = @selector(cocoaWindow:didChangeFocus:);
    if ([delegate respondsToSelector:selector]) {
        [delegate cocoaWindow:self didChangeFocus:NO];
    }
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
    (void)sender;
    self.closeRequested = YES;
    id<RadrayCocoaWindowDelegate> delegate = self.delegate;
    SEL selector = @selector(cocoaWindowDidRequestClose:);
    if ([delegate respondsToSelector:selector]) {
        [delegate cocoaWindowDidRequestClose:self];
    }
    return NO;
}

@end

@implementation RadrayCocoaEventPump {
    NSHashTable<RadrayCocoaWindow*>* _windows;
}

- (instancetype)init {
    self = [super init];
    if (self != nil) {
        _windows = [NSHashTable weakObjectsHashTable];
    }
    return self;
}

- (void)dispatchEvents {
    RadrayCocoaWindowGlobalInit();
    for (;;) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                            untilDate:[NSDate distantPast]
                                               inMode:NSDefaultRunLoopMode
                                              dequeue:YES];
        if (event == nil) {
            break;
        }
        [NSApp sendEvent:event];
    }
    [NSApp updateWindows];
}

- (BOOL)registerWindow:(RadrayCocoaWindow*)window {
    if (window == nil || ![window isValid]) {
        return NO;
    }
    [_windows addObject:window];
    return YES;
}

- (void)unregisterWindow:(RadrayCocoaWindow*)window {
    if (window == nil) {
        return;
    }
    [_windows removeObject:window];
}

@end
