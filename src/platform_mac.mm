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
    // front that handles that one method and forwards every other call to
    // saucer's original delegate untouched.
    //
    // Two cases, handled differently:
    //   * A link click that wants a new window (target="_blank", Cmd-click) has
    //     no opener dependency, so we open it as a new tab in our UI.
    //   * JS window.open() (OAuth sign-in popups, etc.) MUST keep window.opener /
    //     postMessage working, or the flow hangs with a blank popup. We give it a
    //     real child window backed by a WKWebView built from WebKit's own
    //     `configuration` — that preserves the opener relationship. Our tab
    //     webviews are independent smartviews and cannot.
    // Build a real child window for a JS-opened popup (defined below).
    static WKWebView *make_popup(WKWebViewConfiguration *configuration, WKWindowFeatures *features,
                                 WKWebView *opener);
} // namespace mb

// Owns a popup window spawned by window.open(). Closes the window when the page
// calls window.close() (how OAuth popups dismiss themselves) and breaks the
// retain cycle when the user closes it by hand.
@interface MBPopupDelegate : NSObject <WKUIDelegate, NSWindowDelegate>
@end

@implementation MBPopupDelegate {
  @public
    NSWindow *_window; // the popup window we own (retained)
}

- (void)dealloc
{
    [_window release];
    [super dealloc];
}

- (void)webViewDidClose:(WKWebView *)webView // window.close() from the page
{
    [_window close];
}

- (void)windowWillClose:(NSNotification *)notification
{
    // Cycle is window -> contentView(webview) -> associated delegate -> _window.
    // Dropping the content view breaks it so everything can deallocate once
    // WebKit releases the popup web view.
    _window.contentView = nil;
}

// A nested window.open() inside the popup (OAuth flows sometimes chain through a
// relay window) gets its own real child window too, so its opener relationship
// and postMessage path stay intact.
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures
{
    return mb::make_popup(configuration, windowFeatures, webView);
}
@end

namespace mb
{
    // Build a real child window for a JS-opened popup, backed by a WKWebView that
    // uses WebKit's supplied `configuration` (so window.opener / postMessage keep
    // working). WebKit loads the pending navigation into the returned web view.
    static WKWebView *make_popup(WKWebViewConfiguration *configuration, WKWindowFeatures *features,
                                 WKWebView *opener)
    {
        const CGFloat w = features.width ? features.width.doubleValue : 500;
        const CGFloat h = features.height ? features.height.doubleValue : 600;
        const NSRect frame = NSMakeRect(0, 0, w, h);

        // Share the opener's cookie/storage session. Each tab has its own isolated
        // data store (per-profile storage_path); WebKit would otherwise hand the
        // popup a different store, so any CSRF/state cookie the site set before
        // opening the popup would be missing at the OAuth callback — which the site
        // rejects as an invalid authorization code.
        if (opener.configuration.websiteDataStore)
        {
            configuration.websiteDataStore = opener.configuration.websiteDataStore;
        }

        // Allow Web Inspector on the popup so sign-in failures can be diagnosed
        // (right-click -> Inspect Element).
        [configuration.preferences setValue:@YES forKey:@"developerExtrasEnabled"];

        WKWebView *popup      = [[WKWebView alloc] initWithFrame:frame configuration:configuration];
        popup.customUserAgent = opener.customUserAgent; // match opener so sites don't sniff us as outdated

        NSWindow *window = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        window.releasedWhenClosed = NO; // we manage its lifetime via the delegate
        window.contentView        = popup;
        [window center];

        MBPopupDelegate *delegate = [[MBPopupDelegate alloc] init];
        delegate->_window         = [window retain];
        window.delegate           = delegate; // weak
        popup.UIDelegate          = delegate;

        static const char kPopupKey = 0; // pin the delegate to the popup web view
        objc_setAssociatedObject(popup, &kPopupKey, delegate, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        [delegate release];

        [window makeKeyAndOrderFront:nil];

        [popup release];  // retained by the window (contentView) and by WebKit's return
        [window release]; // retained by delegate->_window
        return popup;
    }
} // namespace mb

// Private WebKit SPI carrying the hit-test result for a context-menu request.
// `absoluteLinkURL` is the URL of the link under the cursor, or nil if the
// right-click wasn't on a link. We only message instances WebKit hands us, so
// declaring the known properties is enough — no link-time dependency.
@interface _WKHitTestResult : NSObject
@property (nonatomic, readonly, copy) NSURL *absoluteLinkURL;
@end

@interface _WKContextMenuElementInfo : NSObject
@property (nonatomic, readonly) _WKHitTestResult *hitTestResult;
@end

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
    // A link click (target="_blank", Cmd-click) becomes a tab: no opener needed.
    if (navigationAction.navigationType == WKNavigationTypeLinkActivated)
    {
        NSURL *url = navigationAction.request.URL;
        if (url.absoluteString.length && _open)
        {
            _open(std::string(url.absoluteString.UTF8String));
        }
        return nil; // handled as a tab; don't let WebKit make a detached web view
    }

    // JS window.open() (OAuth sign-in, etc.): give it a real popup window that
    // keeps window.opener / postMessage intact, built from WebKit's config.
    return mb::make_popup(configuration, windowFeatures, webView);
}

// Right-clicking a link: prepend "Open Link in New Tab" to the native context
// menu. WebKit hands us the proposed menu plus a hit-test result via this
// private WKUIDelegate method; we insert our item and return the menu through
// the completion handler. Non-link clicks leave the menu untouched.
- (void)_webView:(WKWebView *)webView
    getContextMenuFromProposedMenu:(NSMenu *)menu
                        forElement:(_WKContextMenuElementInfo *)element
                          userInfo:(id<NSSecureCoding>)userInfo
                 completionHandler:(void (^)(NSMenu *))completionHandler
{
    NSURL *link = element.hitTestResult.absoluteLinkURL;
    if (link.absoluteString.length)
    {
        NSMenuItem *item = [[[NSMenuItem alloc] initWithTitle:@"Open Link in New Tab"
                                                       action:@selector(mbOpenLinkInNewTab:)
                                                keyEquivalent:@""] autorelease];
        item.target           = self;
        item.representedObject = link.absoluteString;
        [menu insertItem:item atIndex:0];
        [menu insertItem:[NSMenuItem separatorItem] atIndex:1];
    }
    completionHandler(menu);
}

- (void)mbOpenLinkInNewTab:(NSMenuItem *)sender
{
    NSString *url = sender.representedObject;
    if (url.length && _open)
    {
        _open(std::string(url.UTF8String));
    }
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
