#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>

extern "C" CAMetalLayer *
RadrayMetalCreateLayer(id<MTLDevice> device, uint64_t windowHandle,
                       uint32_t width, uint32_t height, bool vsync,
                       uint32_t backBufferCount) noexcept {
  NSView *view = nullptr;
  auto window_or_view =
      (__bridge NSObject *)(reinterpret_cast<void *>(windowHandle));
  if ([window_or_view isKindOfClass:[NSWindow class]]) {
    view = static_cast<NSWindow *>(window_or_view).contentView;
  } else {
    if (![window_or_view isKindOfClass:[NSView class]]) {
      NSLog(@"Invalid window handle %llu of class %@. "
             "Expected NSWindow or NSView.",
            windowHandle, [window_or_view class]);
    }
    view = static_cast<NSView *>(window_or_view);
  }
  auto layer = [CAMetalLayer layer];
  view.layer = layer;
  view.wantsLayer = YES;
  view.layer.contentsScale = view.window.backingScaleFactor;
  layer.device = device;
  layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  layer.wantsExtendedDynamicRangeContent = false;
  layer.displaySyncEnabled = vsync;
  layer.maximumDrawableCount = backBufferCount > 3u   ? 3u
                               : backBufferCount < 2u ? 2u
                                                      : backBufferCount;
  layer.drawableSize = CGSizeMake(width, height);
  return layer;
}
