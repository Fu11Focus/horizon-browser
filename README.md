# minibrowser

A minimalist, cross-platform developer browser built in C++ on top of each OS's
**native webview** (no bundled Chromium). Tiny binary, low memory, full native
DevTools.

- **Engine:** system webview via [saucer](https://github.com/saucer/saucer) —
  WebView2 (Windows), WKWebView (macOS), WebKitGTK (Linux).
- **DevTools:** the platform's native inspector (Chrome DevTools on Windows,
  WebKit Web Inspector on macOS, WebKitGTK inspector on Linux).
- **UI:** the chrome (tab strip + toolbar) is a small HTML/CSS/JS page embedded
  into the binary; page content lives in one native webview per tab.
- **Footprint:** ~1–2 MB app bundle; memory is just the shared system webview.

## Architecture

```
┌──────────────────────── native window ───────────────────────┐
│  shell webview  (content/) — tab strip + address bar          │  76px
├───────────────────────────────────────────────────────────────┤
│  content webview — active tab (one per tab, inactive hidden)  │
└───────────────────────────────────────────────────────────────┘
```

The C++ core (`src/main.cpp`) owns the tabs and drives the content webviews.
The shell talks to the core over saucer's JS↔C++ bridge:

- **JS → C++:** `window.saucer.exposed.mb_*()` — `mb_ready`, `mb_navigate`,
  `mb_new_tab`, `mb_close_tab`, `mb_switch_tab`, `mb_back`, `mb_forward`,
  `mb_reload`, `mb_toggle_devtools`.
- **C++ → JS:** `window.mbAddTab / mbRemoveTab / mbSetActive / mbUpdateTab`,
  invoked via `webview.execute(...)` on webview navigation/title/load events.

## Project layout

```
CMakeLists.txt      # build; fetches saucer via CPM, embeds content/ into the binary
cmake/CPM.cmake     # dependency manager
src/main.cpp        # C++ core: window, shell, tabs, bridge
content/            # chrome UI (embedded into the binary at build time)
  index.html
  style.css
  app.js
```

## Build & run (macOS)

Requires CMake, Ninja and a C++23 compiler (Apple clang):

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
open build/minibrowser.app
```

The `content/` UI is compiled into the executable, so the binary is
self-contained — no external asset files at runtime.

## Status

- [x] Toolchain + single webview window
- [x] Shell chrome (tab strip + toolbar) with JS↔C++ bridge
- [x] Address-bar navigation, back / forward / reload, URL+title sync
- [x] Multiple tabs (create / close / switch, state preserved per tab)
- [x] DevTools toggle (toolbar button) + right-click Inspect
- [x] Keyboard shortcuts (⌘T/W/L/R, ⌘\[ / ⌘\], ⌘1–9, ⌘⌥I)
- [x] Chrome-style equal-width tabs (shrink as more open, then scroll)
- [x] Favicons in the tab strip (resolved from the page DOM; works on local sites too)
- [ ] Windows / Linux build verification

## Keyboard shortcuts

| Shortcut            | Action                 |
| ------------------- | ---------------------- |
| ⌘T / Ctrl+T         | New tab                |
| ⌘W / Ctrl+W         | Close current tab      |
| ⌘L / Ctrl+L         | Focus the address bar  |
| ⌘R / Ctrl+R         | Reload                 |
| ⌘\[ / ⌘\]           | Back / Forward         |
| ⌘1 … ⌘8             | Switch to tab 1 … 8    |
| ⌘9                  | Switch to the last tab |
| ⌘⌥I / Ctrl+Alt+I    | Toggle DevTools        |

Shortcuts are captured inside each webview (saucer exposes no window-level key
events) and routed to the C++ core over the bridge.
```
