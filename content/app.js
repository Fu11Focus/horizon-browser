// Chrome UI logic. This page is the browser's shell (tab strip + toolbar).
// It talks to the C++ core through saucer's JS<->C++ bridge:
//   JS -> C++:  window.saucer.exposed.mb_*(...)
//   C++ -> JS:  window.mb* functions below, invoked via webview.execute()
(function () {
  // macOS keeps the traffic-light controls in our top row — reserve space for them.
  if (/Mac/i.test(navigator.platform || navigator.userAgent)) {
    document.documentElement.classList.add("mac");
  }

  const tabs = new Map();      // id -> { url, title }
  let activeId = null;
  let editing = false;         // user is typing in the address bar
  let currentProfile = "";

  const el = (id) => document.getElementById(id);
  const exposed = () => window.saucer && window.saucer.exposed;
  const call = (name, ...args) => {
    const e = exposed();
    if (e && typeof e[name] === "function") return e[name](...args);
  };

  const SVG_NS = "http://www.w3.org/2000/svg";
  function icon(d, cls) {
    const svg = document.createElementNS(SVG_NS, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("class", "ico " + (cls || ""));
    svg.setAttribute("aria-hidden", "true");
    const path = document.createElementNS(SVG_NS, "path");
    path.setAttribute("d", d);
    svg.appendChild(path);
    return svg;
  }

  // Stable per-profile colour for its avatar (white text stays readable on each).
  const AVATAR_COLORS = ["#3d6bff", "#1f8a5f", "#b5561f", "#8a4fd6", "#c23d6b", "#0f7d8c"];
  function avatar(name) {
    const a = document.createElement("span");
    a.className = "avatar";
    let h = 0;
    for (const ch of name) h = (h * 31 + ch.codePointAt(0)) >>> 0;
    a.style.setProperty("--avatar", AVATAR_COLORS[h % AVATAR_COLORS.length]);
    a.textContent = name ? name[0].toUpperCase() : "?";
    return a;
  }

  // Lock icon for https pages, search icon otherwise / while typing.
  function updateAddrIcon() {
    const info = tabs.get(activeId);
    const secure = !editing && !!info && /^https:/i.test(info.url || "");
    el("addr-wrap").classList.toggle("secure", secure);
  }

  function dragRegion(cls) {
    const d = document.createElement("div");
    d.className = cls;
    d.setAttribute("data-webview-drag", "");
    return d;
  }

  function renderTabs() {
    const strip = el("tabs");
    strip.innerHTML = "";
    strip.appendChild(dragRegion("drag-left")); // room for macOS traffic lights
    for (const [id, info] of tabs) {
      const t = document.createElement("div");
      t.className = "tab" + (id === activeId ? " active" : "");
      t.onclick = () => call("mb_switch_tab", id);

      if (info.favicon) {
        const fav = document.createElement("img");
        fav.className = "favicon";
        fav.src = info.favicon;
        fav.onerror = () => fav.replaceWith(blankFavicon());
        t.appendChild(fav);
      } else {
        t.appendChild(blankFavicon());
      }

      const label = document.createElement("span");
      label.className = "label";
      label.textContent = info.title || info.url || "New Tab";

      const close = document.createElement("span");
      close.className = "close";
      close.title = "Close tab";
      close.appendChild(icon("M6 6l12 12M18 6L6 18", "xs"));
      close.onclick = (e) => { e.stopPropagation(); call("mb_close_tab", id); };

      t.appendChild(label);
      t.appendChild(close);
      strip.appendChild(t);
    }
    const add = document.createElement("button");
    add.id = "newtab";
    add.className = "nav";
    add.title = "New tab (⌘T)";
    add.setAttribute("aria-label", "New tab");
    add.appendChild(icon("M12 5v14M5 12h14", "sm"));
    add.onclick = () => call("mb_new_tab");
    strip.appendChild(add);
    strip.appendChild(dragRegion("drag-fill")); // draggable empty space on the right
  }

  function blankFavicon() {
    const b = document.createElement("span");
    b.className = "favicon blank";
    return b;
  }

  // ---- called from C++ ----
  window.mbAddTab = (id, info) => {
    tabs.set(id, { url: (info && info.url) || "", title: (info && info.title) || "", favicon: "" });
    renderTabs();
  };
  window.mbRemoveTab = (id) => { tabs.delete(id); renderTabs(); };
  window.mbSetActive = (id) => {
    activeId = id;
    const info = tabs.get(id);
    if (info && !editing) el("addr").value = info.url || "";
    updateAddrIcon();
    renderTabs();
  };
  window.mbUpdateTab = (id, info) => {
    const t = tabs.get(id);
    if (!t) return;
    if (info.url !== undefined) t.url = info.url;
    if (info.title !== undefined) t.title = info.title;
    if (info.favicon !== undefined) t.favicon = info.favicon;
    if (id === activeId && info.url !== undefined && !editing) {
      el("addr").value = info.url;
    }
    if (id === activeId) updateAddrIcon();
    renderTabs();
  };

  // focus the address bar (⌘L), routed from the C++ core
  window.mbFocusAddr = () => {
    const a = el("addr");
    editing = true;
    updateAddrIcon();
    a.focus();
    a.select();
  };

  // ---- in-page search (driven by the C++ core) ----
  window.mbOpenFind = () => {
    el("find").hidden = false;
    const i = el("find-input");
    i.focus();
    i.select();
    if (i.value) call("mb_find", i.value); // re-run existing query
  };
  window.mbCloseFind = () => {
    el("find").hidden = true;
    el("find-box").classList.remove("nomatch");
  };
  window.mbFindResult = (current, total) => {
    el("find-count").textContent = current + "/" + total;
    const q = el("find-input");
    el("find-box").classList.toggle("nomatch", !!q.value && total === 0);
  };

  // ---- profiles ----
  async function buildChips() {
    const chips = el("profile-chips");
    chips.innerHTML = "";
    let list = [];
    try { list = (await call("mb_profiles")) || []; } catch (e) {}
    for (const name of list) {
      const c = document.createElement("div");
      c.className = "chip" + (name === currentProfile ? " current" : "");
      c.appendChild(avatar(name));
      c.appendChild(document.createTextNode(name));
      if (name !== currentProfile) c.onclick = () => { call("mb_switch_profile", name); call("mb_toggle_profiles"); };
      chips.appendChild(c);
    }
  }
  window.mbOpenProfiles = () => { el("profiles").hidden = false; buildChips(); };
  window.mbCloseProfiles = () => { el("profiles").hidden = true; el("profile-name").value = ""; };

  // ---- save-password banner ----
  window.mbOfferPassword = (origin, user) => {
    const text = el("password-text");
    const site = document.createElement("b");
    site.textContent = origin;
    text.replaceChildren("Save password" + (user ? " for " + user : "") + " on ", site, "?");
    el("password").hidden = false;
  };
  window.mbClosePassword = () => { el("password").hidden = true; };

  function wire() {
    el("back").onclick = () => call("mb_back");
    el("fwd").onclick = () => call("mb_forward");
    el("reload").onclick = () => call("mb_reload");
    el("devtools").onclick = () => call("mb_toggle_devtools");
    el("profile").onclick = () => call("mb_toggle_profiles");

    el("password-save").onclick = () => call("mb_save_password");
    el("password-never").onclick = () => call("mb_never_password");
    el("password-dismiss").onclick = () => call("mb_dismiss_password");

    const createProfile = () => {
      const v = el("profile-name").value.trim();
      if (v) { call("mb_new_profile", v); call("mb_toggle_profiles"); }
    };
    el("profile-create").onclick = createProfile;
    el("profile-window").onclick = () => { call("mb_new_window"); call("mb_toggle_profiles"); };
    el("profile-incognito").onclick = () => { call("mb_new_incognito"); call("mb_toggle_profiles"); };
    el("profile-name").addEventListener("keydown", (e) => {
      if (e.key === "Enter") { e.preventDefault(); createProfile(); }
      if (e.key === "Escape") { e.preventDefault(); call("mb_toggle_profiles"); }
    });

    const addr = el("addr");
    addr.addEventListener("focus", () => { editing = true; updateAddrIcon(); addr.select(); });
    addr.addEventListener("blur", () => { editing = false; updateAddrIcon(); });
    addr.addEventListener("keydown", (e) => {
      if (e.key === "Enter") { editing = false; addr.blur(); call("mb_navigate", addr.value); }
      if (e.key === "Escape") {
        editing = false; addr.blur();
        const info = tabs.get(activeId);
        if (info) addr.value = info.url || "";
      }
    });

    // find bar
    el("find-prev").onclick = () => call("mb_find_prev");
    el("find-next").onclick = () => call("mb_find_next");
    el("find-close").onclick = () => call("mb_find_close");
    const fi = el("find-input");
    fi.addEventListener("input", () => call("mb_find", fi.value));
    fi.addEventListener("keydown", (e) => {
      if (e.key === "Enter") { e.preventDefault(); call(e.shiftKey ? "mb_find_prev" : "mb_find_next"); }
      if (e.key === "Escape") { e.preventDefault(); call("mb_find_close"); }
    });
  }

  async function initProfile() {
    try { currentProfile = (await call("mb_current_profile")) || ""; } catch (e) {}
    let incognito = false;
    try { incognito = !!(await call("mb_is_incognito")); } catch (e) {}
    const btn = el("profile");
    if (incognito) {
      document.documentElement.classList.add("incognito");
      const pill = document.createElement("span");
      pill.className = "avatar";
      pill.append(icon("M3 12s3.5-6 9-6 9 6 9 6-3.5 6-9 6-9-6-9-6zM4 4l16 16", "xs"), "Private");
      btn.replaceChildren(pill);
      btn.title = "Private window";
    } else {
      btn.replaceChildren(avatar(currentProfile));
      btn.title = "Profile: " + (currentProfile || "?");
    }
  }

  function init() {
    if (!exposed()) { setTimeout(init, 30); return; }  // wait for the bridge
    wire();
    call("mb_ready");
    initProfile();
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
