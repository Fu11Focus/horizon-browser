// macOS-only: give the window a Brave/Chrome-style title bar — transparent and
// hidden so the chrome fills the top row, but with the native window controls
// (traffic lights) kept visible. saucer's decoration::partial would hide those
// buttons, so we configure the NSWindow directly via the stable native handle.

#include <saucer/app.hpp>
#include <saucer/window.hpp>
#include <saucer/modules/stable/webkit.hpp>

#import <Security/Security.h>
#import <objc/runtime.h>

#include <functional>
#include <optional>
#include <string>

namespace mb
{
    static NSString *nsstr(const std::string &s)
    {
        return [NSString stringWithUTF8String:s.c_str()];
    }

    // Run a block on the main thread. The legacy file keychain is tied to the
    // thread's session, and saucer runs exposed functions on a worker thread, so
    // keychain access from there fails unless hopped to the main thread.
    static void run_on_main(void (^work)(void))
    {
        if ([NSThread isMainThread])
        {
            work();
        }
        else
        {
            dispatch_sync(dispatch_get_main_queue(), work);
        }
    }

    // Store a secret in the login keychain (generic password), replacing any
    // existing item for (service, account). The app only ever reads back items
    // it created, so no access prompt on read.
    bool keychain_store(const std::string &service, const std::string &account, const std::string &data)
    {
        __block bool ok = false;
        run_on_main(^{
          @autoreleasepool
          {
              NSDictionary *query = @{
                  (id)kSecClass : (id)kSecClassGenericPassword,
                  (id)kSecAttrService : nsstr(service),
                  (id)kSecAttrAccount : nsstr(account),
              };
              SecItemDelete((__bridge CFDictionaryRef)query);

              NSMutableDictionary *add = [query mutableCopy];
              add[(id)kSecValueData]   = [nsstr(data) dataUsingEncoding:NSUTF8StringEncoding];

              ok = SecItemAdd((__bridge CFDictionaryRef)add, nullptr) == errSecSuccess;
          }
        });
        return ok;
    }

    std::optional<std::string> keychain_load(const std::string &service, const std::string &account)
    {
        __block std::optional<std::string> result;
        run_on_main(^{
          @autoreleasepool
          {
              NSDictionary *query = @{
                  (id)kSecClass : (id)kSecClassGenericPassword,
                  (id)kSecAttrService : nsstr(service),
                  (id)kSecAttrAccount : nsstr(account),
                  (id)kSecReturnData : @YES,
                  (id)kSecMatchLimit : (id)kSecMatchLimitOne,
              };
              CFTypeRef out     = nullptr;
              const OSStatus rc = SecItemCopyMatching((__bridge CFDictionaryRef)query, &out);
              if (rc == errSecSuccess && out)
              {
                  NSData *data = (__bridge NSData *)out;
                  result       = std::string(static_cast<const char *>(data.bytes), data.length);
                  CFRelease(out); // SecItemCopyMatching returns a +1 reference (no ARC here)
              }
          }
        });
        return result;
    }

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

    // Quit the app loop. saucer's quit() is just `[NSApp stop:]`, which only
    // takes effect after the next event is processed — when the last window
    // closes there is no such event, so the app hangs idle. Post a dummy event
    // to wake the loop so `-[NSApplication run]` actually returns.
    void wake_and_quit(saucer::application *app)
    {
        @autoreleasepool
        {
            NSApplication *ns = app->native<true>().application;
            [ns stop:nil];
            NSEvent *event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                                location:NSZeroPoint
                                           modifierFlags:0
                                               timestamp:0
                                            windowNumber:0
                                                 context:nil
                                                 subtype:0
                                                   data1:0
                                                   data2:0];
            [ns postEvent:event atStart:YES];
        }
    }

    // --- New-window handling (target="_blank", window.open) -----------------
    //
    // saucer installs its own WKUIDelegate (for JS alert/confirm), but it does
    // not implement createWebViewWithConfiguration:, so WebKit silently drops
    // requests to open a link in a new window. We slot a proxy delegate in
    // front that handles that one method — opening the URL as a new tab — and
    // forwards every other call to saucer's original delegate untouched.
} // namespace mb

@interface MBNewWindowDelegate : NSObject <WKUIDelegate>
@end

@implementation MBNewWindowDelegate {
  @public
    id _inner;                               // saucer's original UIDelegate
    std::function<void(std::string)> _open;  // opens a URL in a new tab
}

- (void)dealloc
{
    [_inner release];
    [super dealloc];
}

- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures
{
    NSURL *url = navigationAction.request.URL;
    if (url.absoluteString.length && _open)
    {
        _open(std::string(url.absoluteString.UTF8String));
    }
    return nil; // handled ourselves; don't let WebKit make a detached web view
}

// Transparently defer anything we don't implement (alert/confirm/prompt, file
// pickers, …) to saucer's delegate so its behaviour is preserved.
- (BOOL)respondsToSelector:(SEL)selector
{
    return [super respondsToSelector:selector] || [_inner respondsToSelector:selector];
}

- (id)forwardingTargetForSelector:(SEL)selector
{
    return [_inner respondsToSelector:selector] ? _inner : [super forwardingTargetForSelector:selector];
}
@end

namespace mb
{
    void install_new_window_handler(saucer::webview *view, std::function<void(std::string)> open)
    {
        @autoreleasepool
        {
            WKWebView *wv = view->native<true>().webview;
            if (!wv)
            {
                return;
            }

            MBNewWindowDelegate *proxy = [[MBNewWindowDelegate alloc] init];
            proxy->_inner              = [wv.UIDelegate retain];
            proxy->_open               = std::move(open);

            // WKWebView.UIDelegate is a weak reference, so pin the proxy's
            // lifetime to the web view via an associated object.
            static const char kKey = 0;
            objc_setAssociatedObject(wv, &kKey, proxy, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            wv.UIDelegate = proxy;
            [proxy release];
        }
    }

    // Wipe all website data (cookies, storage, caches) of a webview's data store.
    // Used to make incognito windows ephemeral: called for each tab on close.
    void wipe_webview_data(saucer::webview *view)
    {
        @autoreleasepool
        {
            WKWebView *wv = view->native<true>().webview;
            if (!wv)
            {
                return;
            }
            WKWebsiteDataStore *store = wv.configuration.websiteDataStore;
            NSSet *types              = [WKWebsiteDataStore allWebsiteDataTypes];
            [store removeDataOfTypes:types modifiedSince:[NSDate distantPast] completionHandler:^{
            }];
        }
    }
} // namespace mb
