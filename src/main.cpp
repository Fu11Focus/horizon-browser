// Horizon — minimalist cross-platform developer browser.
//
// Architecture: a single native window hosts a "shell" webview (the chrome:
// tab strip + toolbar, rendered as an embedded HTML/CSS/JS page) docked at the
// top, plus one "content" webview per tab below it. The C++ core owns the tabs
// and drives them; the shell talks to the core over saucer's JS<->C++ bridge.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
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
    void wipe_webview_data(saucer::webview *);  // src/platform_mac.mm
    void set_page_zoom(saucer::webview *, double); // src/platform_mac.mm
    void wake_and_quit(saucer::application *);   // src/platform_mac.mm
    void install_new_window_handler(saucer::webview *, std::function<void(std::string)>); // src/platform_mac.mm
    bool keychain_store(const std::string &service, const std::string &account, const std::string &data);
    std::optional<std::string> keychain_load(const std::string &service, const std::string &account);
    void keychain_rename_service(const std::string &from, const std::string &to); // migrate a profile's secrets
    bool keychain_delete_service(const std::string &service);                      // drop a profile's secrets
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
        auto dir = base / "Horizon";
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

    // Case-insensitive compare (the macOS filesystem is case-insensitive, so two
    // profile names differing only in case would collide on disk).
    bool iequals(std::string_view a, std::string_view b)
    {
        if (a.size() != b.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            {
                return false;
            }
        }
        return true;
    }

    // Rename a profile's on-disk data directory and migrate its saved passwords.
    // Caller has already validated `to` (sanitized, non-colliding) and ensured the
    // profile is not open in any window. Returns false if the move fails.
    bool rename_profile_data(const std::string &from, const std::string &to)
    {
        std::error_code ec;
        const auto src = profiles_root() / from;
        const auto dst = profiles_root() / to;
        if (!fs::exists(src, ec))
        {
            return false;
        }
        // Block only a collision with a *different* profile; a case-only change
        // (same dir on a case-insensitive FS) is allowed to fall through to rename.
        if (fs::exists(dst, ec) && !iequals(from, to))
        {
            return false;
        }
        fs::rename(src, dst, ec);
        if (ec)
        {
            return false;
        }
#ifdef __APPLE__
        mb::keychain_rename_service("Horizon:" + from, "Horizon:" + to);
#endif
        if (last_profile() == from)
        {
            set_last_profile(to);
        }
        return true;
    }

    // Delete a profile's data directory and its saved passwords. Caller has
    // ensured the profile is not open and is not the only one left.
    bool delete_profile_data(const std::string &name)
    {
        std::error_code ec;
        const auto dir = profiles_root() / name;
        if (!fs::exists(dir, ec))
        {
            return false;
        }
        fs::remove_all(dir, ec);
        if (ec)
        {
            return false;
        }
#ifdef __APPLE__
        mb::keychain_delete_service("Horizon:" + name);
#endif
        if (last_profile() == name)
        {
            const auto remaining = list_profiles(); // recreates "Default" if none remain
            set_last_profile(remaining.empty() ? "Default" : remaining.front());
        }
        return true;
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

    void wipe_webview_data([[maybe_unused]] saucer::webview *view)
    {
#ifdef __APPLE__
        mb::wipe_webview_data(view);
#endif
    }

    void set_page_zoom([[maybe_unused]] saucer::webview *view, [[maybe_unused]] double factor)
    {
#ifdef __APPLE__
        mb::set_page_zoom(view, factor);
#endif
    }

    // Unique, per-window storage label for an incognito window. Saucer hashes the
    // storage_path string into a data-store UUID, so a distinct label gives an
    // isolated session; a random component avoids colliding with a prior session.
    std::string make_incognito_label()
    {
        std::random_device rd;
        std::string label = "incognito-";
        for (int i = 0; i < 4; ++i)
        {
            char buf[9];
            std::snprintf(buf, sizeof(buf), "%08x", rd());
            label += buf;
        }
        return label;
    }

    // User-Agent for content tabs. The WebKit backend's default UA omits a
    // Version/Safari token, so sites that sniff it (e.g. ukr.net) mistake us for
    // an ancient browser and serve a degraded page. Advertise a current Safari.
    constexpr std::string_view kUserAgent =
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
        "(KHTML, like Gecko) Version/18.0 Safari/605.1.15";

    // Height of the chrome strip (tab row + toolbar). Must match content/style.css.
    constexpr int kChromeHeight = 84;

    // Extra height of the find bar when in-page search is open. Must match #find
    // in content/style.css.
    constexpr int kFindHeight = 40;

    // Extra height of the profiles panel when open. Must match #profiles in CSS.
    constexpr int kProfilesHeight = 48;

    // Extra height of the save-password banner when shown. Must match #password.
    constexpr int kPasswordHeight = 48;

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
          case "KeyT": e.preventDefault(); return send(e.shiftKey ? "reopen_tab" : "new_tab");
          case "KeyW": e.preventDefault(); return send("close_tab");
          case "KeyL": e.preventDefault(); return send("focus_addr");
          case "KeyR": e.preventDefault(); return send("reload");
          case "BracketLeft":  e.preventDefault(); return send("back");
          case "BracketRight": e.preventDefault(); return send("forward");
          case "KeyF": e.preventDefault(); return send("find");
          case "KeyN": e.preventDefault(); return send(e.shiftKey ? "new_incognito" : "new_window");
          case "Tab": e.preventDefault(); return send(e.shiftKey ? "prev_tab" : "next_tab");
          // Page zoom. On macOS the unshifted "+" key reports code "Equal"; also
          // accept the numpad keys. Cmd+0 resets (Digit0 is a no-op for tab
          // switching, which only uses 1-9).
          case "Equal":
          case "NumpadAdd":      e.preventDefault(); return send("zoom_in");
          case "Minus":
          case "NumpadSubtract": e.preventDefault(); return send("zoom_out");
          case "Digit0":
          case "Numpad0":        e.preventDefault(); return send("zoom_reset");
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

    // Injected into each content tab. Captures credentials on login-form submit,
    // shows a pick-to-fill suggestion when a login field is focused, and autofills
    // a single saved credential on load. Top frame only; the password only ever
    // crosses the bridge as a function argument (never logged).
    constexpr std::string_view kPasswordScript = R"js(
    (function () {
      if (window.__mbPw) return;
      window.__mbPw = true;

      var cred = null, fetched = false;
      function getCred(cb) {
        if (fetched) { cb(cred); return; }
        try {
          window.saucer.exposed.mb_get_credentials(location.origin).then(function (c) {
            fetched = true; cred = (c && c.found && c.password) ? c : null; cb(cred);
          }, function () { fetched = true; cb(null); });
        } catch (e) { fetched = true; cb(null); }
      }

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

      function fill(anchor) {
        if (!cred) return;
        var form = anchor.form;
        var pw = form ? form.querySelector('input[type="password"]') : document.querySelector('input[type="password"]');
        if (!pw) return;
        pw.value = cred.password;
        var uf = form ? userField(form, pw) : null;
        if (uf && cred.username) uf.value = cred.username;
        [pw, uf].forEach(function (el) {
          if (!el) return;
          el.dispatchEvent(new Event("input", { bubbles: true }));
          el.dispatchEvent(new Event("change", { bubbles: true }));
        });
        hide();
      }

      // ---- pick-to-fill suggestion dropdown (Shadow DOM, inline styles for CSP) ----
      var host = null;
      function hide() { if (host) { host.remove(); host = null; } }
      function css(el, o) { for (var k in o) el.style[k] = o[k]; }
      function show(anchor) {
        hide();
        if (!cred) return;
        var r = anchor.getBoundingClientRect();
        host = document.createElement("div");
        css(host, { position: "fixed", left: r.left + "px", top: (r.bottom + 2) + "px",
                    width: Math.max(r.width, 240) + "px", zIndex: "2147483647" });
        var sh = host.attachShadow({ mode: "open" });
        var box = document.createElement("div");
        css(box, { font: "13px -apple-system, system-ui, sans-serif", background: "#ffffff", color: "#111",
                   border: "1px solid #cfcfcf", borderRadius: "8px", boxShadow: "0 6px 20px rgba(0,0,0,.18)",
                   overflow: "hidden" });
        var row = document.createElement("div");
        css(row, { display: "flex", flexDirection: "column", gap: "2px", padding: "8px 12px", cursor: "default" });
        var u = document.createElement("div"); u.textContent = cred.username || "Saved password";
        css(u, { fontWeight: "600" });
        var p = document.createElement("div"); p.textContent = "••••••••";
        css(p, { color: "#888", letterSpacing: "2px" });
        row.appendChild(u); row.appendChild(p);
        row.addEventListener("mouseenter", function () { row.style.background = "#eef3ff"; });
        row.addEventListener("mouseleave", function () { row.style.background = "transparent"; });
        row.addEventListener("mousedown", function (e) { e.preventDefault(); fill(anchor); });
        box.appendChild(row); sh.appendChild(box);
        (document.body || document.documentElement).appendChild(host);
      }

      function relevant(input) {
        if (!input || input.tagName !== "INPUT") return false;
        if ((input.type || "").toLowerCase() === "password") return true;
        return input.form && input.form.querySelector('input[type="password"]');
      }

      document.addEventListener("focusin", function (e) {
        if (relevant(e.target)) getCred(function (c) { if (c) show(e.target); });
      }, true);
      document.addEventListener("mousedown", function (e) {
        if (host && e.target !== host && !host.contains(e.target)) hide();
      }, true);
      window.addEventListener("scroll", hide, true);
      window.addEventListener("resize", hide, true);
      document.addEventListener("keydown", function (e) { if (e.key === "Escape") hide(); }, true);

      // capture credentials on submit
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

      // autofill a single saved credential on load
      function autofill() {
        getCred(function (c) {
          if (!c) return;
          var pw = document.querySelector('input[type="password"]');
          if (pw && !pw.value) fill(pw);
        });
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

    // ---- session restore: the tabs a profile had open, persisted to disk ----

    struct SessionState
    {
        std::vector<std::string> urls;   // open tab URLs, in order
        std::vector<bool> pinned;        // parallel to urls: was each tab pinned?
        int active = 0;                  // index into urls of the active tab
    };

    SessionState load_session(const std::string &profile)
    {
        SessionState s;
        std::ifstream f(profile_dir(profile) / "session.txt");
        if (!f)
        {
            return s;
        }
        std::string line;
        if (std::getline(f, line)) // first line: active index
        {
            s.active = std::atoi(line.c_str());
        }
        while (std::getline(f, line)) // remaining lines: "<0|1>\t<url>", or a bare url (old format)
        {
            bool pinned = false;
            if (line.size() > 2 && line[1] == '\t' && (line[0] == '0' || line[0] == '1'))
            {
                pinned = line[0] == '1';
                line.erase(0, 2);
            }
            line = trim(line);
            if (!line.empty())
            {
                s.urls.push_back(line);
                s.pinned.push_back(pinned);
            }
        }
        return s;
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
        bool pinned  = false; // pinned tabs are compact and kept at the front of the strip
        double zoom  = 1.0;   // page zoom factor (Cmd +/-/0); re-applied on each navigation
        std::unique_ptr<saucer::smartview> view;
    };

    struct Browser;
    Browser *open_window(saucer::application *app, std::string profile, bool incognito = false,
                         bool restore = false); // defined below

    struct Browser
    {
        saucer::application *app{};
        std::shared_ptr<saucer::window> window;
        std::unique_ptr<saucer::smartview> shell;
        std::string profile       = "Default"; // isolated storage context for this window
        std::string session_label = "";        // storage_path string for this window's tabs
        bool incognito            = false;      // ephemeral session, no password vault

        std::vector<Tab> tabs;
        std::vector<std::string> closed_tabs; // URLs of recently closed tabs (reopen stack)
        int active     = -1;
        int next_id    = 1;
        int width          = 1200;
        int height         = 800;
        bool find_open     = false; // in-page search bar visible (adds kFindHeight to chrome)
        bool profiles_open = false; // profiles panel visible (adds kProfilesHeight)
        bool password_open = false; // save-password banner visible (adds kPasswordHeight)
        bool restoring        = false; // mid session-restore: suppress interim saves
        bool restore_on_ready = false; // restore last session when the shell is ready

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

        // Push a tab's zoom factor into its page. On macOS this uses WebKit's
        // native page zoom, which reflows the layout like a real browser (CSS
        // `zoom` only scales visually and leaves responsive pages clipped). The
        // native factor persists across navigations; other backends fall back to
        // CSS `zoom`, re-applied by the load handler on each page.
        void apply_zoom(Tab &t)
        {
#ifdef __APPLE__
            set_page_zoom(t.view.get(), t.zoom);
#else
            t.view->execute("document.documentElement.style.zoom = {}", t.zoom);
#endif
        }

        // Tell the shell the active tab's zoom level (percent) so it can show /
        // hide the zoom indicator. 100% hides it.
        void report_zoom()
        {
            if (!shell)
            {
                return;
            }
            Tab *t        = find(active);
            const int pct = static_cast<int>((t ? t->zoom : 1.0) * 100.0 + 0.5);
            shell->execute("window.mbSetZoom({})", pct);
        }

        // Step the active tab's zoom. `dir` > 0 zooms in, < 0 out, 0 resets to 100%.
        void change_zoom(int dir)
        {
            Tab *t = find(active);
            if (!t)
            {
                return;
            }

            if (dir == 0)
            {
                t->zoom = 1.0;
            }
            else
            {
                // ~20% geometric steps, clamped to a sane range (Chrome-like).
                const double factor = dir > 0 ? 1.2 : 1.0 / 1.2;
                t->zoom             = std::clamp(t->zoom * factor, 0.25, 5.0);
                // Snap near-100% back to exactly 1.0 so zoom clears cleanly.
                if (t->zoom > 0.99 && t->zoom < 1.01)
                {
                    t->zoom = 1.0;
                }
            }

            apply_zoom(*t);
            report_zoom();
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

        int new_tab(const std::string &url = "", bool focus_addr = false)
        {
            Tab tab;
            tab.id   = next_id++;
            tab.view = std::make_unique<saucer::smartview>(
                saucer::smartview::create({.window             = window,
                                           .persistent_cookies = !incognito,
                                           .storage_path       = fs::path{session_label},
                                           .user_agent         = std::string{kUserAgent}})
                    .value());

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
                {
                    shell->execute("window.mbUpdateTab({}, {{url: {}}})", id, u.string());
                    save_session();
                });

            view->on<saucer::webview::event::load>(
                [this, id](const saucer::state &st)
                {
                    const bool loading = st == saucer::state::started;
                    shell->execute("window.mbUpdateTab({}, {{loading: {}}})", id, loading);
                    // CSS zoom lives on the document and is wiped by each new page,
                    // so re-apply the tab's remembered factor once loading settles.
                    if (!loading)
                    {
                        if (auto *t = find(id); t && t->zoom != 1.0)
                        {
                            apply_zoom(*t);
                        }
                    }
                });

            // Open-in-new-window requests (target="_blank", window.open(),
            // Cmd-click) should become new tabs. These reach us by two different
            // paths in WebKit: a Cmd-click fires the navigate policy event with
            // new_window set, while a plain target="_blank" bypasses that event
            // entirely and only asks the UI delegate to create a web view — which
            // WebKit silently drops when unhandled. Cover both.
            view->on<saucer::webview::event::navigate>(
                [this](const saucer::navigation &nav) -> saucer::policy
                {
                    if (nav.new_window())
                    {
                        new_tab(nav.url().string());
                        return saucer::policy::block;
                    }
                    return saucer::policy::allow;
                });
#ifdef __APPLE__
            mb::install_new_window_handler(view, [this](std::string u) { new_tab(std::move(u)); });
#endif

            tabs.push_back(std::move(tab));
            active = id;

            if (!url.empty()) // empty = blank new tab (nothing to steal address-bar focus)
            {
                view->set_url(make_url(url));
            }
            layout();

            shell->execute("window.mbAddTab({}, {{url: {}, title: \"\"}})", id, url);
            shell->execute("window.mbSetActive({})", id);
            report_zoom();

            if (focus_addr) // put the cursor in the address bar, ready to type
            {
                focus_webview(shell.get());
                shell->execute("window.mbFocusAddr && window.mbFocusAddr()");
            }

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

            if (const std::string url = it->view->url().string(); !url.empty()) // remember for ⇧⌘T
            {
                closed_tabs.push_back(url);
                if (closed_tabs.size() > 25)
                {
                    closed_tabs.erase(closed_tabs.begin());
                }
            }

            tabs.erase(it); // destroys the content webview
            shell->execute("window.mbRemoveTab({})", id);

            if (tabs.empty())
            {
                save_session();    // nothing left; persist the empty state
                new_tab("", true); // never leave the window empty
                return;
            }

            if (was_active)
            {
                const int ni = std::min(idx, static_cast<int>(tabs.size()) - 1);
                active       = tabs[ni].id;
                shell->execute("window.mbSetActive({})", active);
                report_zoom();
            }

            layout();
            save_session();
        }

        void reopen_tab() // ⇧⌘T: reopen the most recently closed tab
        {
            if (closed_tabs.empty())
            {
                return;
            }
            const std::string url = closed_tabs.back();
            closed_tabs.pop_back();
            new_tab(url);
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
            report_zoom();
            save_session();
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

        std::string pw_service() const { return "Horizon:" + profile; }

        void offer_credentials(std::string origin, std::string username, std::string password)
        {
            if (incognito || origin.empty() || is_never(profile, origin))
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
            if (incognito)
            {
                return c; // incognito never autofills from a profile's vault
            }
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

        void cycle_tab(int dir) // Ctrl+Tab / Ctrl+Shift+Tab: next / previous, wrapping
        {
            if (tabs.empty())
            {
                return;
            }
            const int n = static_cast<int>(tabs.size());
            int idx     = 0;
            for (int i = 0; i < n; ++i)
            {
                if (tabs[i].id == active)
                {
                    idx = i;
                    break;
                }
            }
            switch_tab(tabs[(idx + dir + n) % n].id);
        }

        void reorder_tabs(const std::vector<int> &ids) // match the shell's drag-reordered order
        {
            std::vector<Tab> out;
            out.reserve(tabs.size());
            for (int id : ids)
            {
                for (auto &t : tabs)
                {
                    if (t.view && t.id == id)
                    {
                        out.push_back(std::move(t));
                        break;
                    }
                }
            }
            for (auto &t : tabs) // keep any not mentioned (safety)
            {
                if (t.view)
                {
                    out.push_back(std::move(t));
                }
            }
            tabs = std::move(out);
            // Enforce the invariant that pinned tabs stay at the front (the shell
            // keeps them there too, so this is just a safety net).
            std::stable_partition(tabs.begin(), tabs.end(), [](const Tab &t) { return t.pinned; });
            save_session();
        }

        // Tell the shell the current tab order (ids), so it can resync its strip
        // after the core reorders tabs (e.g. on pin/unpin or session restore).
        void notify_order()
        {
            std::vector<int> ids;
            ids.reserve(tabs.size());
            for (auto &t : tabs)
            {
                ids.push_back(t.id);
            }
            shell->execute("window.mbReorder({})", ids);
        }

        void set_pinned(int id, bool pinned)
        {
            auto *t = find(id);
            if (!t || t->pinned == pinned)
            {
                return;
            }
            t->pinned = pinned;
            // Pinned tabs move to the front; unpinned fall back behind them. A
            // stable partition keeps the relative order within each group.
            std::stable_partition(tabs.begin(), tabs.end(), [](const Tab &x) { return x.pinned; });
            shell->execute("window.mbUpdateTab({}, {{pinned: {}}})", id, pinned);
            notify_order();
            save_session();
        }

        // ---- session restore: persist this window's tabs per profile ----

        void save_session()
        {
            if (incognito || restoring)
            {
                return;
            }
            std::vector<std::string> urls;
            std::vector<bool> pins;
            int active_idx = 0;
            for (auto &t : tabs)
            {
                const std::string u = t.view->url().string();
                if (u.empty())
                {
                    continue; // blank new tab — nothing worth restoring
                }
                if (t.id == active)
                {
                    active_idx = static_cast<int>(urls.size());
                }
                urls.push_back(u);
                pins.push_back(t.pinned);
            }
            std::ofstream f(profile_dir(profile) / "session.txt", std::ios::trunc);
            f << active_idx << "\n";
            for (std::size_t i = 0; i < urls.size(); ++i)
            {
                f << (pins[i] ? '1' : '0') << '\t' << urls[i] << "\n";
            }
        }

        // Reopen the tabs saved from this profile's last session. Returns false if
        // there is nothing to restore (the caller then opens a blank tab instead).
        bool restore_session()
        {
            const auto st = load_session(profile);
            if (st.urls.empty())
            {
                return false;
            }
            restoring = true; // suppress interim saves; async navigations persist later
            for (std::size_t i = 0; i < st.urls.size(); ++i)
            {
                const int id = new_tab(st.urls[i]);
                if (st.pinned[i])
                {
                    if (auto *t = find(id))
                    {
                        t->pinned = true;
                    }
                }
            }
            // Pinned tabs first, then push the pin flags + order to the shell.
            std::stable_partition(tabs.begin(), tabs.end(), [](const Tab &t) { return t.pinned; });
            for (auto &t : tabs)
            {
                if (t.pinned)
                {
                    shell->execute("window.mbUpdateTab({}, {{pinned: {}}})", t.id, true);
                }
            }
            notify_order();
            const int idx = (st.active >= 0 && st.active < static_cast<int>(tabs.size())) ? st.active : 0;
            if (!tabs.empty())
            {
                switch_tab(tabs[idx].id);
            }
            restoring = false;
            return true;
        }

        // Install keyboard-shortcut capture + routing on a webview (shell or tab).
        void install_shortcuts(saucer::smartview *view)
        {
            view->expose("mb_key", [this](std::string key) { on_key(key); });
            view->inject({.code = std::string{kShortcutScript}, .run_at = saucer::script::time::creation});
        }

        void on_key(const std::string &k)
        {
            if (k == "new_tab")          new_tab("", true); // focus the address bar, like the "+" button
            else if (k == "reopen_tab")  reopen_tab();
            else if (k == "close_tab")   { if (active != -1) close_tab(active); }
            else if (k == "reload")      reload();
            else if (k == "back")        back();
            else if (k == "forward")     forward();
            else if (k == "devtools")    toggle_devtools();
            else if (k == "find")        open_find();
            else if (k == "new_window")  open_window(app, profile);
            else if (k == "new_incognito") open_window(app, profile, true);
            else if (k == "focus_addr")  { if (shell) { focus_webview(shell.get()); shell->execute("window.mbFocusAddr && window.mbFocusAddr()"); } }
            else if (k == "next_tab")    cycle_tab(1);
            else if (k == "prev_tab")    cycle_tab(-1);
            else if (k == "zoom_in")     change_zoom(1);
            else if (k == "zoom_out")    change_zoom(-1);
            else if (k == "zoom_reset")  change_zoom(0);
            else if (k.rfind("tab:", 0) == 0) switch_to_index(std::atoi(k.c_str() + 4));
        }
    };

    // All open browser windows (one per profile instance). A window's `closed`
    // event removes its entry (destroying the webviews while the app run loop is
    // alive, which is safe).
    //
    // Intentionally heap-allocated and never freed: any Browser still present at
    // program exit must NOT be destroyed during static destruction.
    // saucer::webview::~webview() blocks on a future serviced by WebKit helper
    // threads, which are already stopped by the time `exit()` runs atexit
    // destructors — so tearing a surviving window down there deadlocks (observed
    // as a spinning-cursor hang after the last window closes). Leaking the
    // container avoids that; the OS reclaims the memory and kills the WebKit
    // child processes on exit anyway.
    std::vector<std::unique_ptr<Browser>> &g_browsers = *new std::vector<std::unique_ptr<Browser>>();

    // Is a (non-incognito) profile currently open in some window? Renaming or
    // deleting a profile whose data store is live would corrupt it, so the UI
    // forbids it while open.
    bool profile_is_open(const std::string &name)
    {
        for (const auto &p : g_browsers)
        {
            if (p && !p->incognito && p->profile == name)
            {
                return true;
            }
        }
        return false;
    }

    Browser *open_window(saucer::application *app, std::string profile, bool incognito, bool restore)
    {
        auto owned = std::make_unique<Browser>();
        auto *b    = owned.get();

        b->app             = app;
        b->incognito       = incognito;
        b->restore_on_ready = restore && !incognito;
        b->profile   = incognito ? "Incognito" : profile;
        // Each window's tabs share one storage label; incognito uses a unique
        // ephemeral one (isolated session), profiles use their on-disk dir.
        b->session_label = incognito ? make_incognito_label() : profile_dir(profile).string();
        b->window        = saucer::window::create(app).value();

        b->window->set_title("Horizon — " + b->profile);
        b->window->set_size({.w = b->width, .h = b->height});

#ifndef __APPLE__
        // Windows/Linux: the window icon comes from the embedded PNG (macOS uses the bundle's icon.icns).
        if (auto icon = saucer::icon::from(saucer::embedded::all().at("/icon.png").content); icon)
        {
            b->window->set_icon(*icon);
        }
#endif

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
                if (b->incognito)
                {
                    for (auto &t : b->tabs) // make the ephemeral session leave nothing behind
                    {
                        wipe_webview_data(t.view.get());
                    }
                }
                app->post(
                    [app, b]
                    {
                        std::erase_if(g_browsers, [&](const std::unique_ptr<Browser> &p) { return p.get() == b; });
                        if (g_browsers.empty())
                        {
#ifdef __APPLE__
                            mb::wake_and_quit(app); // [NSApp stop:] alone won't wake an idle loop
#else
                            app->quit();
#endif
                        }
                    });
            });

        // Bridge: shell UI -> C++ core.
        b->shell->expose("mb_ready",
                         [b]
                         {
                             if (!b->tabs.empty())
                             {
                                 return;
                             }
                             if (b->restore_on_ready && b->restore_session())
                             {
                                 return;
                             }
                             b->new_tab("", true);
                         });
        b->shell->expose("mb_navigate", [b](std::string u) { b->navigate(u); });
        b->shell->expose("mb_new_tab", [b] { b->new_tab("", true); });
        b->shell->expose("mb_close_tab", [b](int id) { b->close_tab(id); });
        b->shell->expose("mb_switch_tab", [b](int id) { b->switch_tab(id); });
        b->shell->expose("mb_reorder_tabs", [b](std::vector<int> ids) { b->reorder_tabs(ids); });
        b->shell->expose("mb_set_pinned", [b](int id, bool pinned) { b->set_pinned(id, pinned); });
        b->shell->expose("mb_back", [b] { b->back(); });
        b->shell->expose("mb_forward", [b] { b->forward(); });
        b->shell->expose("mb_reload", [b] { b->reload(); });
        b->shell->expose("mb_zoom_reset", [b] { b->change_zoom(0); });
        b->shell->expose("mb_toggle_devtools", [b] { b->toggle_devtools(); });
        b->shell->expose("mb_find", [b](std::string q) { b->find_query(std::move(q)); });
        b->shell->expose("mb_find_next", [b] { b->find_next(); });
        b->shell->expose("mb_find_prev", [b] { b->find_prev(); });
        b->shell->expose("mb_find_close", [b] { b->close_find(); });

        // Bridge: profiles.
        b->shell->expose("mb_profiles", [] { return list_profiles(); });
        b->shell->expose("mb_current_profile", [b] { return b->profile; });
        b->shell->expose("mb_toggle_profiles", [b] { b->toggle_profiles(); });
        b->shell->expose("mb_new_window", [app, b] { open_window(app, b->profile, b->incognito); });
        b->shell->expose("mb_new_incognito", [app, b] { open_window(app, b->profile, true); });
        b->shell->expose("mb_is_incognito", [b] { return b->incognito; });

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

        // Rename / delete a profile. Both return "" on success or a short,
        // user-facing error string that the shell shows inline on the chip.
        b->shell->expose("mb_rename_profile",
                         [](std::string from, std::string to) -> std::string
                         {
                             const auto clean = sanitize_profile(to);
                             if (clean.empty())
                             {
                                 return "Enter a valid name";
                             }
                             if (clean == from)
                             {
                                 return ""; // no-op
                             }
                             if (profile_is_open(from))
                             {
                                 return "Close its window first";
                             }
                             const auto all = list_profiles();
                             for (const auto &n : all) // reject collision with a *different* profile
                             {
                                 if (iequals(n, clean) && !iequals(n, from))
                                 {
                                     return "That name is taken";
                                 }
                             }
                             return rename_profile_data(from, clean) ? std::string{} : std::string{"Couldn't rename"};
                         });
        b->shell->expose("mb_delete_profile",
                         [](std::string name) -> std::string
                         {
                             if (profile_is_open(name))
                             {
                                 return "Close its window first";
                             }
                             if (list_profiles().size() <= 1)
                             {
                                 return "Can't delete the only profile";
                             }
                             return delete_profile_data(name) ? std::string{} : std::string{"Couldn't delete"};
                         });

        b->shell->embed(saucer::embedded::all());
        b->shell->serve("/index.html");
        b->layout();
        b->window->show();

        if (!incognito)
        {
            set_last_profile(profile);
        }

        g_browsers.push_back(std::move(owned));
        return b;
    }
} // namespace

coco::stray start(saucer::application *app)
{
    list_profiles(); // ensure a Default profile exists
    open_window(app, last_profile(), /*incognito=*/false, /*restore=*/true);

    co_await app->finish();
}

int main()
{
    return saucer::application::create({.id = "Horizon"})->run(start);
}
