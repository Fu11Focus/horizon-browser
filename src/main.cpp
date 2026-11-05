// minibrowser — minimalist cross-platform developer browser.
//
// Architecture: a single native window hosts a "shell" webview (the chrome:
// tab strip + toolbar, rendered as an embedded HTML/CSS/JS page) docked at the
// top, plus one "content" webview per tab below it. The C++ core owns the tabs
// and drives them; the shell talks to the core over saucer's JS<->C++ bridge.

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <saucer/smartview.hpp>
#include <saucer/embedded/all.hpp>

namespace
{
    // Height of the chrome strip (tab row + toolbar). Must match content/style.css.
    constexpr int kChromeHeight = 76;

    // Injected into every webview (shell + each tab). Captures browser keyboard
    // shortcuts and forwards them to the C++ core. saucer exposes no window-level
    // key events, so we intercept them in-page instead. Uses e.code so it is
    // keyboard-layout independent; accepts Cmd (macOS) or Ctrl (Win/Linux).
    constexpr std::string_view kShortcutScript = R"js(
    (function () {
      if (window.__mbKeys) return;
      window.__mbKeys = true;
      function send(k) { try { window.saucer.exposed.mb_key(k); } catch (e) {} }
      window.addEventListener("keydown", function (e) {
        if (!(e.metaKey || e.ctrlKey)) return;
        var c = e.code;
        if (e.altKey) {
          if (c === "KeyI") { e.preventDefault(); send("devtools"); }
          return;
        }
        switch (c) {
          case "KeyT": e.preventDefault(); return send("new_tab");
          case "KeyW": e.preventDefault(); return send("close_tab");
          case "KeyL": e.preventDefault(); return send("focus_addr");
          case "KeyR": e.preventDefault(); return send("reload");
          case "BracketLeft":  e.preventDefault(); return send("back");
          case "BracketRight": e.preventDefault(); return send("forward");
        }
        if (c && c.indexOf("Digit") === 0) {
          var n = parseInt(c.slice(5), 10);
          if (n >= 1 && n <= 9) { e.preventDefault(); send("tab:" + n); }
        }
      }, true);
    })();
    )js";

    // Injected into each content tab. Resolves the page's favicon URL from the
    // DOM and reports it to the core (more reliable and backend-independent than
    // the native favicon event, and works for the user's own local sites). Runs
    // at document end on every page load.
    constexpr std::string_view kFaviconScript = R"js(
    (function () {
      function pick() {
        var ls = document.querySelectorAll(
          'link[rel~="icon"], link[rel="shortcut icon"], link[rel="apple-touch-icon"]');
        var href = "";
        for (var i = 0; i < ls.length; i++) { if (ls[i].href) href = ls[i].href; }
        if (!href && location.origin && location.origin !== "null") {
          href = location.origin + "/favicon.ico";
        }
        return href;
      }
      function report() { try { var h = pick(); if (h) window.saucer.exposed.mb_favicon(h); } catch (e) {} }
      report();
      window.addEventListener("load", report);
    })();
    )js";

    std::string trim(std::string_view s)
    {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string_view::npos)
        {
            return {};
        }
        const auto e = s.find_last_not_of(" \t\r\n");
        return std::string{s.substr(b, e - b + 1)};
    }

    // Turn free-form address-bar input into a real URL: pass-through when it
    // already has a scheme, prepend https:// for bare hosts, otherwise treat it
    // as a web search.
    saucer::url make_url(std::string_view input)
    {
        const std::string s = trim(input);
        std::string full;

        if (s.find("://") != std::string::npos)
        {
            full = s;
        }
        else
        {
            const bool has_space = s.find(' ') != std::string::npos;
            const bool has_dot   = s.find('.') != std::string::npos;

            if (s.empty() || has_space || !has_dot)
            {
                std::string q;
                for (char c : s)
                {
                    q += (c == ' ') ? std::string{"%20"} : std::string(1, c);
                }
                full = "https://www.google.com/search?q=" + q;
            }
            else
            {
                full = "https://" + s;
            }
        }

        if (auto parsed = saucer::url::parse(full); parsed.has_value())
        {
            return std::move(parsed.value());
        }

        return saucer::url::parse("https://example.com/").value();
    }

    struct Tab
    {
        int id;
        std::unique_ptr<saucer::smartview> view;
    };

    struct Browser
    {
        saucer::application *app{};
        std::shared_ptr<saucer::window> window;
        std::unique_ptr<saucer::smartview> shell;

        std::vector<Tab> tabs;
        int active  = -1;
        int next_id = 1;
        int width   = 1200;
        int height  = 800;

        Tab *find(int id)
        {
            for (auto &t : tabs)
            {
                if (t.id == id)
                {
                    return &t;
                }
            }
            return nullptr;
        }

        // Place a view using a top-left logical origin. saucer's WKWebView backend
        // (macOS) sets frames in Cocoa's bottom-left coordinate space, so flip Y
        // there; WebView2 (Windows) and WebKitGTK (Linux) already use top-left.
        void place(saucer::webview *view, int x, int y_top, int w, int h)
        {
#ifdef __APPLE__
            const int y = height - y_top - h;
#else
            const int y = y_top;
#endif
            view->set_bounds({.x = x, .y = y, .w = w, .h = h});
        }

        void layout()
        {
            if (window)
            {
                const auto sz = window->size(); // content-area size (authoritative)
                width         = sz.w;
                height        = sz.h;
            }

            if (shell)
            {
                place(shell.get(), 0, 0, width, kChromeHeight);
            }

            for (auto &t : tabs)
            {
                if (t.id == active)
                {
                    place(t.view.get(), 0, kChromeHeight, width, height - kChromeHeight);
                }
                else
                {
                    t.view->set_bounds({.x = 0, .y = 0, .w = 0, .h = 0}); // hidden
                }
            }
        }

        int new_tab(const std::string &url = "https://www.google.com/")
        {
            Tab tab;
            tab.id   = next_id++;
            tab.view = std::make_unique<saucer::smartview>(saucer::smartview::create({.window = window}).value());

            const int id = tab.id;
            auto *view   = tab.view.get();

            view->set_context_menu(true);
            install_shortcuts(view);

            view->expose("mb_favicon",
                         [this, id](std::string href)
                         { shell->execute("window.mbUpdateTab({}, {{favicon: {}}})", id, href); });
            view->inject({.code = std::string{kFaviconScript}, .run_at = saucer::script::time::ready});

            view->on<saucer::webview::event::title>(
                [this, id](std::string_view title)
                { shell->execute("window.mbUpdateTab({}, {{title: {}}})", id, std::string{title}); });

            view->on<saucer::webview::event::navigated>(
                [this, id](const saucer::url &u)
                { shell->execute("window.mbUpdateTab({}, {{url: {}}})", id, u.string()); });

            view->on<saucer::webview::event::load>(
                [this, id](const saucer::state &st)
                {
                    const bool loading = st == saucer::state::started;
                    shell->execute("window.mbUpdateTab({}, {{loading: {}}})", id, loading);
                });

            tabs.push_back(std::move(tab));
            active = id;

            view->set_url(make_url(url));
            layout();

            shell->execute("window.mbAddTab({}, {{url: {}, title: \"\"}})", id, url);
            shell->execute("window.mbSetActive({})", id);

            return id;
        }

        void close_tab(int id)
        {
            const auto it = std::find_if(tabs.begin(), tabs.end(), [&](const Tab &t) { return t.id == id; });
            if (it == tabs.end())
            {
                return;
            }

            const bool was_active = active == id;
            const int  idx        = static_cast<int>(it - tabs.begin());

            tabs.erase(it); // destroys the content webview
            shell->execute("window.mbRemoveTab({})", id);

            if (tabs.empty())
            {
                new_tab(); // never leave the window empty
                return;
            }

            if (was_active)
            {
                const int ni = std::min(idx, static_cast<int>(tabs.size()) - 1);
                active       = tabs[ni].id;
                shell->execute("window.mbSetActive({})", active);
            }

            layout();
        }

        void switch_tab(int id)
        {
            if (!find(id))
            {
                return;
            }
            active = id;
            layout();
            shell->execute("window.mbSetActive({})", id);
        }

        void navigate(std::string_view input)
        {
            if (auto *t = find(active))
            {
                t->view->set_url(make_url(input));
            }
        }

        void back()    { if (auto *t = find(active)) t->view->back(); }
        void forward() { if (auto *t = find(active)) t->view->forward(); }
        void reload()  { if (auto *t = find(active)) t->view->reload(); }

        void toggle_devtools()
        {
            if (auto *t = find(active))
            {
                t->view->set_dev_tools(!t->view->dev_tools());
            }
        }

        void switch_to_index(int n) // 1-based; 9 means "last"
        {
            if (tabs.empty())
            {
                return;
            }
            const int idx = (n == 9) ? static_cast<int>(tabs.size()) - 1 : n - 1;
            if (idx >= 0 && idx < static_cast<int>(tabs.size()))
            {
                switch_tab(tabs[idx].id);
            }
        }

        // Install keyboard-shortcut capture + routing on a webview (shell or tab).
        void install_shortcuts(saucer::smartview *view)
        {
            view->expose("mb_key", [this](std::string key) { on_key(key); });
            view->inject({.code = std::string{kShortcutScript}, .run_at = saucer::script::time::creation});
        }

        void on_key(const std::string &k)
        {
            if (k == "new_tab")          new_tab();
            else if (k == "close_tab")   { if (active != -1) close_tab(active); }
            else if (k == "reload")      reload();
            else if (k == "back")        back();
            else if (k == "forward")     forward();
            else if (k == "devtools")    toggle_devtools();
            else if (k == "focus_addr")  { if (shell) shell->execute("window.mbFocusAddr && window.mbFocusAddr()"); }
            else if (k.rfind("tab:", 0) == 0) switch_to_index(std::atoi(k.c_str() + 4));
        }
    };
} // namespace

coco::stray start(saucer::application *app)
{
    Browser browser;
    browser.app    = app;
    browser.window = saucer::window::create(app).value();

    browser.window->set_title("minibrowser");
    browser.window->set_size({.w = browser.width, .h = browser.height});

    browser.shell =
        std::make_unique<saucer::smartview>(saucer::smartview::create({.window = browser.window}).value());

    browser.install_shortcuts(browser.shell.get());

    browser.window->on<saucer::window::event::resize>([&browser](int, int) { browser.layout(); });

    // Bridge: shell UI -> C++ core.
    browser.shell->expose("mb_ready", [&browser] { if (browser.tabs.empty()) browser.new_tab(); });
    browser.shell->expose("mb_navigate", [&browser](std::string u) { browser.navigate(u); });
    browser.shell->expose("mb_new_tab", [&browser] { browser.new_tab(); });
    browser.shell->expose("mb_close_tab", [&browser](int id) { browser.close_tab(id); });
    browser.shell->expose("mb_switch_tab", [&browser](int id) { browser.switch_tab(id); });
    browser.shell->expose("mb_back", [&browser] { browser.back(); });
    browser.shell->expose("mb_forward", [&browser] { browser.forward(); });
    browser.shell->expose("mb_reload", [&browser] { browser.reload(); });
    browser.shell->expose("mb_toggle_devtools", [&browser] { browser.toggle_devtools(); });

    browser.shell->embed(saucer::embedded::all());
    browser.shell->serve("/index.html");
    browser.layout();

    browser.window->show();

    co_await app->finish();
}

int main()
{
    return saucer::application::create({.id = "minibrowser"})->run(start);
}
