// minibrowser — minimalist cross-platform developer browser.
//
// Architecture: a single native window hosts a "shell" webview (the chrome:
// tab strip + toolbar, rendered as an embedded HTML/CSS/JS page) docked at the
// top, plus one "content" webview per tab below it. The C++ core owns the tabs
// and drives them; the shell talks to the core over saucer's JS<->C++ bridge.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <saucer/smartview.hpp>
#include <saucer/embedded/all.hpp>

#include <optional>

// Returned across the JS bridge (serialized by glaze), so it must have external
// linkage — keep it out of the anonymous namespace.
struct Credential
{
    std::string username;
    std::string password;
    bool found = false;
};

#ifdef __APPLE__
namespace mb
{
    void configure_titlebar(saucer::window *); // src/platform_mac.mm
    void focus_webview(saucer::webview *);      // src/platform_mac.mm
    bool keychain_store(const std::string &service, const std::string &account, const std::string &data);
    std::optional<std::string> keychain_load(const std::string &service, const std::string &account);
}
#endif

namespace
{
    namespace fs = std::filesystem;

    // ---- profiles: each profile is an isolated storage directory ----

    fs::path app_data_dir()
    {
#if defined(__APPLE__)
        const char *home = std::getenv("HOME");
        fs::path base    = home ? fs::path(home) / "Library" / "Application Support" : fs::temp_directory_path();
#elif defined(_WIN32)
        const char *appdata = std::getenv("APPDATA");
        fs::path base       = appdata ? fs::path(appdata) : fs::temp_directory_path();
#else
        const char *xdg  = std::getenv("XDG_DATA_HOME");
        const char *home = std::getenv("HOME");
        fs::path base    = xdg ? fs::path(xdg) : (home ? fs::path(home) / ".local" / "share" : fs::temp_directory_path());
#endif
        auto dir = base / "minibrowser";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

    fs::path profiles_root()
    {
        auto dir = app_data_dir() / "profiles";
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

    fs::path profile_dir(const std::string &name)
    {
        auto dir = profiles_root() / name;
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }

    // Keep profile names filesystem-safe and human-friendly.
    std::string sanitize_profile(std::string_view in)
    {
        std::string out;
        for (char c : in)
        {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_')
            {
                out += c;
            }
        }
        const auto b = out.find_first_not_of(' ');
        const auto e = out.find_last_not_of(' ');
        out          = (b == std::string::npos) ? std::string{} : out.substr(b, e - b + 1);
        return out.substr(0, 40);
    }

    std::vector<std::string> list_profiles()
    {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(profiles_root(), ec))
        {
            if (entry.is_directory())
            {
                out.push_back(entry.path().filename().string());
            }
        }
        if (out.empty())
        {
            profile_dir("Default");
            out.emplace_back("Default");
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    std::string last_profile()
    {
        std::ifstream f(app_data_dir() / "last_profile");
        std::string name;
        std::getline(f, name);
        return name.empty() ? "Default" : name;
    }

    void set_last_profile(const std::string &name)
    {
        std::ofstream f(app_data_dir() / "last_profile", std::ios::trunc);
        f << name;
    }

    // ---- saved passwords: secrets go to the OS keychain, never to disk/logs ----

    struct PendingCred
    {
        std::string origin;
        std::string username;
        std::string password;
    };

    bool secret_store(const std::string &service, const std::string &account, const std::string &data)
    {
#ifdef __APPLE__
        return mb::keychain_store(service, account, data);
#else
        // Non-macOS: in-memory only for now (not persisted). A libsecret /
        // Credential Manager backend is TODO for the Windows/Linux port.
        static std::map<std::string, std::string> mem;
        mem[service + "\x1f" + account] = data;
        return true;
#endif
    }

    std::optional<std::string> secret_load(const std::string &service, const std::string &account)
    {
#ifdef __APPLE__
        return mb::keychain_load(service, account);
#else
        static std::map<std::string, std::string> &mem = *new std::map<std::string, std::string>();
        auto it = mem.find(service + "\x1f" + account);
        return it == mem.end() ? std::nullopt : std::optional<std::string>{it->second};
#endif
    }

    // Origins the user chose never to save, one per line under the profile dir.
    bool is_never(const std::string &profile, const std::string &origin)
    {
        std::ifstream f(profile_dir(profile) / "never_passwords.txt");
        std::string line;
        while (std::getline(f, line))
        {
            if (line == origin)
            {
                return true;
            }
        }
        return false;
    }

    void add_never(const std::string &profile, const std::string &origin)
    {
        if (is_never(profile, origin))
        {
            return;
        }
        std::ofstream f(profile_dir(profile) / "never_passwords.txt", std::ios::app);
        f << origin << "\n";
    }

    void focus_webview([[maybe_unused]] saucer::webview *view)
    {
#ifdef __APPLE__
        mb::focus_webview(view);
#endif
    }

    // Height of the chrome strip (tab row + toolbar). Must match content/style.css.
    constexpr int kChromeHeight = 80;

    // Extra height of the find bar when in-page search is open. Must match #find
    // in content/style.css.
    constexpr int kFindHeight = 40;

    // Extra height of the profiles panel when open. Must match #profiles in CSS.
    constexpr int kProfilesHeight = 46;

    // Extra height of the save-password banner when shown. Must match #password.
    constexpr int kPasswordHeight = 44;

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
          case "KeyF": e.preventDefault(); return send("find");
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

    // Injected into each content tab. In-page search: highlights matches, tracks
    // a current match, and reports (current, total) back to the core. Exposed as
    // window.__mbFind.{search,next,prev,clear}; the core drives it via execute().
    constexpr std::string_view kFindScript = R"js(
    (function () {
      if (window.__mbFind) return;
      var MARK = "mb-find-mark", CUR = "mb-find-current";
      var matches = [], idx = -1, styled = false;

      function ensureStyle() {
        if (styled) return;
        styled = true;
        var s = document.createElement("style");
        s.textContent = "." + MARK + "{background:#ffd54a;color:#000} ." + CUR + "{background:#ff9800;color:#000}";
        (document.head || document.documentElement).appendChild(s);
      }
      function report() {
        try { window.saucer.exposed.mb_find_result(matches.length ? idx + 1 : 0, matches.length); } catch (e) {}
      }
      function clear() {
        for (var i = 0; i < matches.length; i++) {
          var m = matches[i], p = m.parentNode;
          if (p) { p.replaceChild(document.createTextNode(m.textContent), m); p.normalize(); }
        }
        matches = []; idx = -1;
      }
      function focusCurrent() {
        for (var i = 0; i < matches.length; i++) matches[i].classList.remove(CUR);
        if (idx >= 0 && idx < matches.length) {
          matches[idx].classList.add(CUR);
          matches[idx].scrollIntoView({ block: "center", inline: "nearest" });
        }
      }
      function search(q) {
        clear();
        if (!q) { report(); return; }
        ensureStyle();
        var needle = q.toLowerCase();
        var walker = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT, {
          acceptNode: function (n) {
            if (!n.nodeValue || !n.nodeValue.trim()) return NodeFilter.FILTER_REJECT;
            var p = n.parentNode; if (!p) return NodeFilter.FILTER_REJECT;
            var t = p.nodeName;
            if (t === "SCRIPT" || t === "STYLE" || t === "NOSCRIPT" || t === "TEXTAREA") return NodeFilter.FILTER_REJECT;
            return NodeFilter.FILTER_ACCEPT;
          }
        });
        var nodes = []; while (walker.nextNode()) nodes.push(walker.currentNode);
        for (var k = 0; k < nodes.length; k++) {
          var node = nodes[k], text = node.nodeValue, lower = text.toLowerCase();
          var pos = lower.indexOf(needle); if (pos === -1) continue;
          var frag = document.createDocumentFragment(), last = 0;
          while (pos !== -1) {
            if (pos > last) frag.appendChild(document.createTextNode(text.slice(last, pos)));
            var mk = document.createElement("mark");
            mk.className = MARK; mk.textContent = text.slice(pos, pos + needle.length);
            frag.appendChild(mk); matches.push(mk);
            last = pos + needle.length; pos = lower.indexOf(needle, last);
          }
          if (last < text.length) frag.appendChild(document.createTextNode(text.slice(last)));
          node.parentNode.replaceChild(frag, node);
        }
        if (matches.length) { idx = 0; focusCurrent(); }
        report();
      }
      function step(d) { if (matches.length) { idx = (idx + d + matches.length) % matches.length; focusCurrent(); report(); } }

      window.__mbFind = {
        search: search,
        next: function () { step(1); },
        prev: function () { step(-1); },
        clear: function () { clear(); report(); }
      };
    })();
    )js";

    // Injected into each content tab. Captures credentials on login-form submit
    // and autofills saved ones on load. Runs in the top frame only. The password
    // only ever crosses the bridge as a function argument (never logged).
    constexpr std::string_view kPasswordScript = R"js(
    (function () {
      if (window.__mbPw) return;
      window.__mbPw = true;

      function userField(form, pw) {
        var inputs = form.querySelectorAll("input"), cand = null;
        for (var i = 0; i < inputs.length; i++) {
          var el = inputs[i];
          if (el === pw) break;
          var t = (el.type || "").toLowerCase();
          if (t === "text" || t === "email" || t === "tel" || t === "" || el.autocomplete === "username") cand = el;
        }
        return cand;
      }

      document.addEventListener("submit", function (e) {
        try {
          var form = e.target;
          if (!form || form.nodeName !== "FORM") return;
          var pw = form.querySelector('input[type="password"]');
          if (!pw || !pw.value) return;
          var uf = userField(form, pw);
          window.saucer.exposed.mb_offer_credentials(location.origin, uf ? uf.value : "", pw.value);
        } catch (err) {}
      }, true);

      async function autofill() {
        try {
          var cred = await window.saucer.exposed.mb_get_credentials(location.origin);
          if (!cred || !cred.found || !cred.password) return;
          var pw = document.querySelector('input[type="password"]');
          if (!pw) return;
          pw.value = cred.password;
          var uf = pw.form ? userField(pw.form, pw) : null;
          if (uf && !uf.value && cred.username) uf.value = cred.username;
          [pw, uf].forEach(function (el) {
            if (!el) return;
            el.dispatchEvent(new Event("input", { bubbles: true }));
            el.dispatchEvent(new Event("change", { bubbles: true }));
          });
        } catch (e) {}
      }
      if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", autofill);
      else autofill();
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
        std::string profile = "Default"; // isolated storage context for this window

        std::vector<Tab> tabs;
        int active     = -1;
        int next_id    = 1;
        int width          = 1200;
        int height         = 800;
        bool find_open     = false; // in-page search bar visible (adds kFindHeight to chrome)
        bool profiles_open = false; // profiles panel visible (adds kProfilesHeight)
        bool password_open = false; // save-password banner visible (adds kPasswordHeight)

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

            const int chrome = kChromeHeight + (find_open ? kFindHeight : 0) +
                               (profiles_open ? kProfilesHeight : 0) + (password_open ? kPasswordHeight : 0);

            if (shell)
            {
                place(shell.get(), 0, 0, width, chrome);
            }

            for (auto &t : tabs)
            {
                if (t.id == active)
                {
                    place(t.view.get(), 0, chrome, width, height - chrome);
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
            tab.view = std::make_unique<saucer::smartview>(
                saucer::smartview::create({.window = window, .storage_path = profile_dir(profile)}).value());

            const int id = tab.id;
            auto *view   = tab.view.get();

            view->set_context_menu(true);
            install_shortcuts(view);

            view->expose("mb_favicon",
                         [this, id](std::string href)
                         { shell->execute("window.mbUpdateTab({}, {{favicon: {}}})", id, href); });
            view->inject({.code = std::string{kFaviconScript}, .run_at = saucer::script::time::ready});

            view->expose("mb_find_result",
                         [this](int current, int total)
                         { shell->execute("window.mbFindResult({}, {})", current, total); });
            view->inject({.code = std::string{kFindScript}, .run_at = saucer::script::time::creation});

            view->expose("mb_offer_credentials",
                         [this](std::string origin, std::string user, std::string pass)
                         { offer_credentials(std::move(origin), std::move(user), std::move(pass)); });
            view->expose("mb_get_credentials", [this](std::string origin) { return get_credentials(origin); });
            view->inject({.code = std::string{kPasswordScript}, .run_at = saucer::script::time::creation});

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
            close_find(); // find state is per-page; don't carry it across tabs
            active = id;
            layout();
            shell->execute("window.mbSetActive({})", id);
        }

        // ---- in-page search ----

        void open_find()
        {
            find_open = true;
            layout();
            shell->execute("window.mbOpenFind()");
            if (shell)
            {
                focus_webview(shell.get()); // so the find input receives typing
            }
        }

        void close_find()
        {
            if (!find_open)
            {
                return;
            }
            find_open = false;
            if (auto *t = find(active))
            {
                t->view->execute("window.__mbFind && window.__mbFind.clear()");
            }
            shell->execute("window.mbCloseFind()");
            layout();
        }

        void find_query(std::string q)
        {
            if (auto *t = find(active))
            {
                t->view->execute("window.__mbFind && window.__mbFind.search({})", std::move(q));
            }
        }

        void find_next() { if (auto *t = find(active)) t->view->execute("window.__mbFind && window.__mbFind.next()"); }
        void find_prev() { if (auto *t = find(active)) t->view->execute("window.__mbFind && window.__mbFind.prev()"); }

        // ---- profiles panel ----

        // ---- saved passwords ----

        std::optional<PendingCred> pending; // credential awaiting a save decision

        std::string pw_service() const { return "minibrowser:" + profile; }

        void offer_credentials(std::string origin, std::string username, std::string password)
        {
            if (origin.empty() || is_never(profile, origin))
            {
                return;
            }
            // Don't nag if the exact same credential is already stored.
            if (auto existing = secret_load(pw_service(), origin))
            {
                const auto nl = existing->find('\n');
                const std::string u = nl == std::string::npos ? std::string{} : existing->substr(0, nl);
                const std::string p = nl == std::string::npos ? *existing : existing->substr(nl + 1);
                if (u == username && p == password)
                {
                    return;
                }
            }
            pending       = PendingCred{std::move(origin), std::move(username), std::move(password)};
            password_open = true;
            layout();
            shell->execute("window.mbOfferPassword({}, {})", pending->origin, pending->username);
        }

        void close_password()
        {
            password_open = false;
            layout();
            shell->execute("window.mbClosePassword()");
        }

        void save_password()
        {
            if (pending)
            {
                secret_store(pw_service(), pending->origin, pending->username + "\n" + pending->password);
                pending.reset();
            }
            close_password();
        }

        void never_password()
        {
            if (pending)
            {
                add_never(profile, pending->origin);
                pending.reset();
            }
            close_password();
        }

        void dismiss_password()
        {
            pending.reset();
            close_password();
        }

        Credential get_credentials(const std::string &origin)
        {
            Credential c;
            if (auto blob = secret_load(pw_service(), origin))
            {
                const auto nl = blob->find('\n');
                c.username    = nl == std::string::npos ? std::string{} : blob->substr(0, nl);
                c.password    = nl == std::string::npos ? *blob : blob->substr(nl + 1);
                c.found       = true;
            }
            return c;
        }

        void toggle_profiles()
        {
            profiles_open = !profiles_open;
            layout();
            if (profiles_open)
            {
                shell->execute("window.mbOpenProfiles()");
            }
            else
            {
                shell->execute("window.mbCloseProfiles()");
            }
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
            else if (k == "find")        open_find();
            else if (k == "focus_addr")  { if (shell) { focus_webview(shell.get()); shell->execute("window.mbFocusAddr && window.mbFocusAddr()"); } }
            else if (k.rfind("tab:", 0) == 0) switch_to_index(std::atoi(k.c_str() + 4));
        }
    };

    // All open browser windows (one per profile instance). Kept alive here for
    // the lifetime of their window; a window's `closed` event removes its entry.
    std::vector<std::unique_ptr<Browser>> g_browsers;

    Browser *open_window(saucer::application *app, std::string profile)
    {
        auto owned = std::make_unique<Browser>();
        auto *b    = owned.get();

        b->app     = app;
        b->profile = profile;
        b->window  = saucer::window::create(app).value();

        b->window->set_title("minibrowser — " + profile);
        b->window->set_size({.w = b->width, .h = b->height});

        // Chrome/Brave-style top row: hide the title bar but keep the window
        // controls. Draggable regions are marked `data-webview-drag` in the shell.
#ifdef __APPLE__
        mb::configure_titlebar(b->window.get());
#endif

        b->shell = std::make_unique<saucer::smartview>(saucer::smartview::create({.window = b->window}).value());
        b->install_shortcuts(b->shell.get());

        b->window->on<saucer::window::event::resize>([b](int, int) { b->layout(); });

        // When the window closes, drop its Browser (deferred, so we are not
        // destroying the window from inside its own event). Quit on the last one.
        b->window->on<saucer::window::event::closed>(
            [app, b]
            {
                app->post(
                    [app, b]
                    {
                        std::erase_if(g_browsers, [&](const std::unique_ptr<Browser> &p) { return p.get() == b; });
                        if (g_browsers.empty())
                        {
                            app->quit();
                        }
                    });
            });

        // Bridge: shell UI -> C++ core.
        b->shell->expose("mb_ready", [b] { if (b->tabs.empty()) b->new_tab(); });
        b->shell->expose("mb_navigate", [b](std::string u) { b->navigate(u); });
        b->shell->expose("mb_new_tab", [b] { b->new_tab(); });
        b->shell->expose("mb_close_tab", [b](int id) { b->close_tab(id); });
        b->shell->expose("mb_switch_tab", [b](int id) { b->switch_tab(id); });
        b->shell->expose("mb_back", [b] { b->back(); });
        b->shell->expose("mb_forward", [b] { b->forward(); });
        b->shell->expose("mb_reload", [b] { b->reload(); });
        b->shell->expose("mb_toggle_devtools", [b] { b->toggle_devtools(); });
        b->shell->expose("mb_find", [b](std::string q) { b->find_query(std::move(q)); });
        b->shell->expose("mb_find_next", [b] { b->find_next(); });
        b->shell->expose("mb_find_prev", [b] { b->find_prev(); });
        b->shell->expose("mb_find_close", [b] { b->close_find(); });

        // Bridge: profiles.
        b->shell->expose("mb_profiles", [] { return list_profiles(); });
        b->shell->expose("mb_current_profile", [b] { return b->profile; });
        b->shell->expose("mb_toggle_profiles", [b] { b->toggle_profiles(); });

        // Bridge: saved passwords.
        b->shell->expose("mb_save_password", [b] { b->save_password(); });
        b->shell->expose("mb_never_password", [b] { b->never_password(); });
        b->shell->expose("mb_dismiss_password", [b] { b->dismiss_password(); });
        b->shell->expose("mb_switch_profile",
                         [app](std::string name)
                         {
                             const auto all = list_profiles();
                             if (std::find(all.begin(), all.end(), name) != all.end())
                             {
                                 open_window(app, name);
                             }
                         });
        b->shell->expose("mb_new_profile",
                         [app](std::string name)
                         {
                             const auto clean = sanitize_profile(name);
                             if (!clean.empty())
                             {
                                 profile_dir(clean);
                                 open_window(app, clean);
                             }
                         });

        b->shell->embed(saucer::embedded::all());
        b->shell->serve("/index.html");
        b->layout();
        b->window->show();

        set_last_profile(profile);

        g_browsers.push_back(std::move(owned));
        return b;
    }
} // namespace

coco::stray start(saucer::application *app)
{
    list_profiles(); // ensure a Default profile exists
    open_window(app, last_profile());

    co_await app->finish();
}

int main()
{
    return saucer::application::create({.id = "minibrowser"})->run(start);
}
