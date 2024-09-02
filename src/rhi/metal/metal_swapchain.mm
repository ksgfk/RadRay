#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

NSUInteger _clamp__(NSUInteger v, NSUInteger a, NSUInteger b) noexcept { return (v > b) ? b : ((v < a) ? a : v); }

extern "C" CAMetalLayer* RadrayMetalCreateLayer(
    id<MTLDevice> device,
    size_t windowHandle,
    uint32_t width,
    uint32_t height,
    uint32_t backBufferCount,
    bool isHdr,
    bool enableSync) {
    NSView* view = nullptr;
    auto windowOrView = (__bridge NSObject*)(reinterpret_cast<void*>(windowHandle));
    if ([windowOrView isKindOfClass:[NSWindow class]]) {
        view = static_cast<NSWindow*>(windowOrView).contentView;
    } else {
        if (![windowOrView isKindOfClass:[NSView class]]) {
            NSLog(@"Invalid window handle %lu of class %@. "
                "Expected NSWindow or NSView.",
                windowHandle, [windowOrView class]);
        }
        view = static_cast<NSView*>(windowOrView);
    }
    CAMetalLayer* layer = [CAMetalLayer layer];
    layer.contentsScale = view.window.backingScaleFactor;
    layer.device = device;
    layer.pixelFormat = isHdr ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
    layer.wantsExtendedDynamicRangeContent = false;
    layer.displaySyncEnabled = enableSync;
    layer.maximumDrawableCount = _clamp__(backBufferCount, 2u, 3u);
    layer.drawableSize = CGSizeMake(width, height);
    view.layer = layer;
    view.wantsLayer = YES;
    return layer;
}
