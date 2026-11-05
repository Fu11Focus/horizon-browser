// macOS-only: give the window a Brave/Chrome-style title bar — transparent and
// hidden so the chrome fills the top row, but with the native window controls
// (traffic lights) kept visible. saucer's decoration::partial would hide those
// buttons, so we configure the NSWindow directly via the stable native handle.

#include <saucer/window.hpp>
#include <saucer/modules/stable/webkit.hpp>

namespace mb
{
    void configure_titlebar(saucer::window *window)
    {
        @autoreleasepool
        {
            NSWindow *w = window->native<true>().window;
            if (!w)
            {
                return;
            }

            w.titlebarAppearsTransparent = YES;
            w.titleVisibility            = NSWindowTitleHidden;
            w.styleMask |= NSWindowStyleMaskFullSizeContentView;

            // Keep the traffic lights visible and functional.
            [w standardWindowButton:NSWindowCloseButton].hidden       = NO;
            [w standardWindowButton:NSWindowMiniaturizeButton].hidden = NO;
            [w standardWindowButton:NSWindowZoomButton].hidden        = NO;
        }
    }
} // namespace mb
