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

    // Give keyboard focus to a specific webview. Needed because the chrome and
    // page content are separate webviews: when a shortcut fires inside the page,
    // focusing the shell's address/find input requires making the shell webview
    // the window's first responder first.
    void focus_webview(saucer::webview *view)
    {
        @autoreleasepool
        {
            WKWebView *wv = view->native<true>().webview;
            if (!wv)
            {
                return;
            }
            [wv.window makeFirstResponder:wv];
        }
    }
} // namespace mb
