#pragma once

#ifndef __OBJC__
#error "radray/window/cocoa/cocoa_window_objc.h is Objective-C only."
#endif

#import <Cocoa/Cocoa.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, RadrayCocoaWindowShowMode) {
    RadrayCocoaWindowShowModeDefault = 0,
    RadrayCocoaWindowShowModeNoActivate = 1,
};

typedef NS_ENUM(NSInteger, RadrayCocoaWindowAction) {
    RadrayCocoaWindowActionUnknown = 0,
    RadrayCocoaWindowActionReleased = 1,
    RadrayCocoaWindowActionPressed = 2,
    RadrayCocoaWindowActionRepeated = 3,
};

typedef NS_ENUM(NSInteger, RadrayCocoaMouseButton) {
    RadrayCocoaMouseButtonUnknown = 0,
    RadrayCocoaMouseButton1 = 1,
    RadrayCocoaMouseButton2 = 2,
    RadrayCocoaMouseButton3 = 3,
    RadrayCocoaMouseButton4 = 4,
    RadrayCocoaMouseButton5 = 5,
    RadrayCocoaMouseButton6 = 6,
    RadrayCocoaMouseButton7 = 7,
    RadrayCocoaMouseButton8 = 8,
    RadrayCocoaMouseButtonLeft = RadrayCocoaMouseButton1,
    RadrayCocoaMouseButtonRight = RadrayCocoaMouseButton2,
    RadrayCocoaMouseButtonMiddle = RadrayCocoaMouseButton3,
};

@class RadrayCocoaWindow;

typedef struct RadrayCocoaWindowDescriptor {
    __unsafe_unretained NSString* _Nullable title;
    __unsafe_unretained RadrayCocoaWindow* _Nullable ownerWindow;
    CGFloat width;
    CGFloat height;
    CGFloat x;
    CGFloat y;
    BOOL resizable;
    BOOL startMaximized;
    BOOL fullscreen;
    BOOL startVisible;
    BOOL decorated;
    BOOL showInTaskbar;
    BOOL topMost;
    BOOL activateOnShow;
    BOOL focusOnClick;
    BOOL inputPassthrough;
} RadrayCocoaWindowDescriptor;

FOUNDATION_EXPORT RadrayCocoaWindowDescriptor RadrayCocoaWindowDescriptorMakeDefault(void);
FOUNDATION_EXPORT void RadrayCocoaWindowGlobalInit(void);
FOUNDATION_EXPORT void RadrayCocoaWindowGlobalShutdown(void);

@protocol RadrayCocoaWindowDelegate <NSObject>
@optional
- (void)cocoaWindow:(RadrayCocoaWindow*)window didResizeToContentSize:(NSSize)size;
- (void)cocoaWindow:(RadrayCocoaWindow*)window didMoveToContentPosition:(NSPoint)position;
- (void)cocoaWindow:(RadrayCocoaWindow*)window didReceiveMouseButton:(RadrayCocoaMouseButton)button action:(RadrayCocoaWindowAction)action position:(NSPoint)position;
- (void)cocoaWindow:(RadrayCocoaWindow*)window didMoveMouseToPosition:(NSPoint)position button:(RadrayCocoaMouseButton)button;
- (void)cocoaWindow:(RadrayCocoaWindow*)window didScrollWithDeltaX:(CGFloat)deltaX deltaY:(CGFloat)deltaY;
- (void)cocoaWindow:(RadrayCocoaWindow*)window didReceiveKeyCode:(unsigned short)keyCode action:(RadrayCocoaWindowAction)action;
- (void)cocoaWindow:(RadrayCocoaWindow*)window didReceiveTextInput:(NSString*)text;
- (void)cocoaWindow:(RadrayCocoaWindow*)window didChangeFocus:(BOOL)focused;
- (void)cocoaWindowDidRequestClose:(RadrayCocoaWindow*)window;
- (void)cocoaWindowDidMouseLeave:(RadrayCocoaWindow*)window;
@end

@interface RadrayCocoaWindow : NSObject

- (nullable instancetype)initWithDescriptor:(RadrayCocoaWindowDescriptor)descriptor NS_DESIGNATED_INITIALIZER;
- (instancetype)init NS_UNAVAILABLE;

@property(nonatomic, weak, nullable) id<RadrayCocoaWindowDelegate> delegate;
@property(nonatomic, readonly, nullable) NSWindow* nsWindow;
@property(nonatomic, readonly, nullable) NSView* contentView;

- (BOOL)isValid;
- (void)destroy;
- (void)show;
- (void)showWithMode:(RadrayCocoaWindowShowMode)mode;
- (void)focus;

- (BOOL)shouldClose;
- (NSSize)contentSize;
- (NSPoint)contentPosition;
- (CGFloat)dpiScale;
- (BOOL)isMinimized;
- (BOOL)isFocused;

- (void)setContentSize:(NSSize)size;
- (void)setContentPosition:(NSPoint)position;
- (void)setTitle:(NSString*)title;
- (void)setAlpha:(CGFloat)alpha;
- (void)setOwnerWindow:(nullable RadrayCocoaWindow*)ownerWindow;
- (void)setDecorated:(BOOL)value;
- (void)setShowInTaskbar:(BOOL)value;
- (void)setTopMost:(BOOL)value;
- (void)setFocusOnClick:(BOOL)value;
- (void)setInputPassthrough:(BOOL)value;

- (NSPoint)clientToScreen:(NSPoint)point;
- (NSPoint)screenToClient:(NSPoint)point;

@end

@interface RadrayCocoaEventPump : NSObject

- (void)dispatchEvents;
- (BOOL)registerWindow:(RadrayCocoaWindow*)window;
- (void)unregisterWindow:(RadrayCocoaWindow*)window;

@end

NS_ASSUME_NONNULL_END
