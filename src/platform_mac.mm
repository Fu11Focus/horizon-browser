// macOS-only: give the window a Brave/Chrome-style title bar — transparent and
// hidden so the chrome fills the top row, but with the native window controls
// (traffic lights) kept visible. saucer's decoration::partial would hide those
// buttons, so we configure the NSWindow directly via the stable native handle.

#include <saucer/window.hpp>
#include <saucer/modules/stable/webkit.hpp>

#import <Security/Security.h>

#include <optional>
#include <string>

namespace mb
{
    static NSString *nsstr(const std::string &s)
    {
        return [NSString stringWithUTF8String:s.c_str()];
    }

    // Store a secret in the login keychain (generic password), replacing any
    // existing item for (service, account). The app only ever reads back items
    // it created, so no access prompt on read.
    bool keychain_store(const std::string &service, const std::string &account, const std::string &data)
    {
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

            return SecItemAdd((__bridge CFDictionaryRef)add, nullptr) == errSecSuccess;
        }
    }

    std::optional<std::string> keychain_load(const std::string &service, const std::string &account)
    {
        @autoreleasepool
        {
            NSDictionary *query = @{
                (id)kSecClass : (id)kSecClassGenericPassword,
                (id)kSecAttrService : nsstr(service),
                (id)kSecAttrAccount : nsstr(account),
                (id)kSecReturnData : @YES,
                (id)kSecMatchLimit : (id)kSecMatchLimitOne,
            };
            CFTypeRef out      = nullptr;
            const OSStatus rc  = SecItemCopyMatching((__bridge CFDictionaryRef)query, &out);
            if (rc != errSecSuccess || !out)
            {
                return std::nullopt;
            }
            NSData *data = (__bridge_transfer NSData *)out;
            return std::string(static_cast<const char *>(data.bytes), data.length);
        }
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
} // namespace mb
