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
        fav.onerror = () => fav.remove();
        t.appendChild(fav);
      }

      const label = document.createElement("span");
      label.className = "label";
      label.textContent = info.title || info.url || "New Tab";

      const close = document.createElement("span");
      close.className = "close";
      close.textContent = "×";
      close.onclick = (e) => { e.stopPropagation(); call("mb_close_tab", id); };

      t.appendChild(label);
      t.appendChild(close);
      strip.appendChild(t);
    }
    strip.appendChild(dragRegion("drag-fill")); // draggable empty space on the right
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
    renderTabs();
  };

  // focus the address bar (⌘L), routed from the C++ core
  window.mbFocusAddr = () => {
    const a = el("addr");
    editing = true;
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
    el("find-input").classList.remove("nomatch");
  };
  window.mbFindResult = (current, total) => {
    el("find-count").textContent = current + "/" + total;
    const q = el("find-input");
    q.classList.toggle("nomatch", !!q.value && total === 0);
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
      c.textContent = name;
      if (name !== currentProfile) c.onclick = () => { call("mb_switch_profile", name); call("mb_toggle_profiles"); };
      chips.appendChild(c);
    }
  }
  window.mbOpenProfiles = () => { el("profiles").hidden = false; buildChips(); };
  window.mbCloseProfiles = () => { el("profiles").hidden = true; el("profile-name").value = ""; };

  // ---- save-password banner ----
  window.mbOfferPassword = (origin, user) => {
    el("password-text").textContent =
      "Save password" + (user ? " for " + user : "") + " on " + origin + "?";
    el("password").hidden = false;
  };
  window.mbClosePassword = () => { el("password").hidden = true; };

  function wire() {
    el("back").onclick = () => call("mb_back");
    el("fwd").onclick = () => call("mb_forward");
    el("reload").onclick = () => call("mb_reload");
    el("newtab").onclick = () => call("mb_new_tab");
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
    el("profile-name").addEventListener("keydown", (e) => {
      if (e.key === "Enter") { e.preventDefault(); createProfile(); }
      if (e.key === "Escape") { e.preventDefault(); call("mb_toggle_profiles"); }
    });

    const addr = el("addr");
    addr.addEventListener("focus", () => { editing = true; addr.select(); });
    addr.addEventListener("blur", () => { editing = false; });
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
    const btn = el("profile");
    btn.textContent = currentProfile ? currentProfile[0].toUpperCase() : "●";
    btn.title = "Profile: " + (currentProfile || "?");
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
