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
  let dragging = false;        // a tab is being dragged to reorder
  let suppressClick = false;   // swallow the click that ends a drag

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
    if (dragging) return; // don't rebuild the strip mid-drag (it would reset the order)
    const strip = el("tabs");
    strip.innerHTML = "";
    strip.appendChild(dragRegion("drag-left")); // room for macOS traffic lights
    for (const [id, info] of tabs) {
      const t = document.createElement("div");
      t.className = "tab" + (id === activeId ? " active" : "") + (info.pinned ? " pinned" : "");
      t.dataset.id = id;
      if (info.pinned) t.title = info.title || info.url || ""; // no label when pinned — show on hover
      t.onclick = () => { if (!suppressClick) call("mb_switch_tab", id); };
      t.addEventListener("pointerdown", (e) => beginDrag(e, t));
      t.addEventListener("contextmenu", (e) => { e.preventDefault(); showTabMenu(e, id); });

      if (info.favicon) {
        const fav = document.createElement("img");
        fav.className = "favicon";
        fav.src = info.favicon;
        fav.onerror = () => fav.replaceWith(blankFavicon());
        t.appendChild(fav);
      } else {
        t.appendChild(blankFavicon());
      }

      if (!info.pinned) { // pinned tabs are favicon-only (no label, no close button)
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
      }
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

  // ---- drag to reorder tabs (within this window) ----
  // Listeners live on document (capture phase), not the tab, so the drag keeps
  // tracking even after the pointer leaves the tab — no reliance on pointer
  // capture, which doesn't behave consistently in the webview.
  function beginDrag(startEvent, t) {
    if (startEvent.button !== 0 || startEvent.target.closest(".close")) return;
    const strip = el("tabs");
    const startX = startEvent.clientX;
    let started = false;

    function onMove(e) {
      if (!started) {
        if (Math.abs(e.clientX - startX) < 5) return; // threshold: distinguish from a click
        started = true;
        dragging = true;
        t.classList.add("dragging");
      }
      e.preventDefault();
      // Keep the drag within its own group: a pinned tab stays among pinned
      // tabs, an unpinned tab among unpinned ones.
      const isPinned = !!(tabs.get(+t.dataset.id) || {}).pinned;
      const siblings = [...strip.querySelectorAll(".tab")].filter((x) => x !== t);
      const inGroup = (x) => !!(tabs.get(+x.dataset.id) || {}).pinned === isPinned;
      const after = siblings.filter(inGroup).find((s) => {
        const r = s.getBoundingClientRect();
        return e.clientX < r.left + r.width / 2;
      });
      // No same-group tab to the right: a pinned tab stops before the first
      // unpinned tab; an unpinned tab stops before the "+" button.
      const ref = after ||
        (isPinned ? siblings.find((x) => !inGroup(x)) : null) || el("newtab");
      strip.insertBefore(t, ref); // keep within the tab run
    }
    function onUp() {
      document.removeEventListener("pointermove", onMove, true);
      document.removeEventListener("pointerup", onUp, true);
      if (started) {
        t.classList.remove("dragging");
        dragging = false;
        commitOrder();
        suppressClick = true;                       // swallow the trailing click
        setTimeout(() => { suppressClick = false; }, 0);
      }
    }
    document.addEventListener("pointermove", onMove, true);
    document.addEventListener("pointerup", onUp, true);
  }

  function commitOrder() {
    let ids = [...el("tabs").querySelectorAll(".tab")].map((x) => +x.dataset.id);
    // Pinned tabs always precede unpinned ones (stable within each group); the
    // core enforces the same invariant.
    const pinned = (i) => !!(tabs.get(i) || {}).pinned;
    ids = ids.filter(pinned).concat(ids.filter((i) => !pinned(i)));
    const next = new Map();
    ids.forEach((i) => { if (tabs.has(i)) next.set(i, tabs.get(i)); });
    tabs.clear();
    next.forEach((v, k) => tabs.set(k, v));
    call("mb_reorder_tabs", ids);
    renderTabs();
  }

  // ---- lightweight context menu (shared by tabs and profile chips) ----
  function closeMenu() {
    const m = el("ctx");
    if (m) m.remove();
  }
  // items: [{ label, fn, danger? }]
  function showMenu(e, items) {
    closeMenu();
    const m = document.createElement("div");
    m.id = "ctx";
    for (const spec of items) {
      const it = document.createElement("div");
      it.className = "ctx-item" + (spec.danger ? " danger" : "");
      it.textContent = spec.label;
      it.onclick = () => { closeMenu(); spec.fn(); };
      m.appendChild(it);
    }
    document.body.appendChild(m);
    // Keep the menu on-screen.
    let x = e.clientX, y = e.clientY;
    if (x + m.offsetWidth > window.innerWidth) x = window.innerWidth - m.offsetWidth - 4;
    if (y + m.offsetHeight > window.innerHeight) y = window.innerHeight - m.offsetHeight - 4;
    m.style.left = x + "px";
    m.style.top = y + "px";
  }

  function showTabMenu(e, id) {
    const info = tabs.get(id);
    if (!info) return;
    showMenu(e, [
      { label: info.pinned ? "Unpin tab" : "Pin tab", fn: () => call("mb_set_pinned", id, !info.pinned) },
      { label: "Close tab", fn: () => call("mb_close_tab", id) },
    ]);
  }

  // ---- called from C++ ----
  window.mbAddTab = (id, info) => {
    tabs.set(id, { url: (info && info.url) || "", title: (info && info.title) || "", favicon: "", pinned: !!(info && info.pinned) });
    renderTabs();
  };
  window.mbRemoveTab = (id) => { tabs.delete(id); renderTabs(); };
  // Reorder the strip to match the core (pin/unpin, session restore).
  window.mbReorder = (ids) => {
    const next = new Map();
    (ids || []).forEach((i) => { if (tabs.has(i)) next.set(i, tabs.get(i)); });
    tabs.forEach((v, k) => { if (!next.has(k)) next.set(k, v); }); // keep any not listed
    tabs.clear();
    next.forEach((v, k) => tabs.set(k, v));
    renderTabs();
  };
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
    if (info.pinned !== undefined) t.pinned = info.pinned;
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
      const label = document.createElement("span");
      label.className = "chip-name";
      label.textContent = name;
      c.appendChild(label);

      // The current profile is open in this window, so it can't be edited here;
      // every other profile gets switch-on-click plus a rename/delete menu.
      if (name !== currentProfile) {
        c.onclick = () => { call("mb_switch_profile", name); call("mb_toggle_profiles"); };
        const openMenu = (e) => showMenu(e, [
          { label: "Rename", fn: () => startRename(c, name) },
          { label: "Delete", danger: true, fn: () => startDelete(c, name) },
        ]);
        c.addEventListener("contextmenu", (e) => { e.preventDefault(); e.stopPropagation(); openMenu(e); });
        const more = document.createElement("button");
        more.className = "chip-more";
        more.title = "Edit profile";
        more.appendChild(icon("M6 12h.01M12 12h.01M18 12h.01", "xs"));
        more.onclick = (e) => { e.stopPropagation(); openMenu(e); };
        c.appendChild(more);
      }
      chips.appendChild(c);
    }
  }

  // Replace a chip's contents with its name in an editable field.
  function startRename(chip, name) {
    chip.onclick = null;
    chip.classList.add("editing");
    chip.innerHTML = "";
    const input = document.createElement("input");
    input.type = "text";
    input.className = "chip-input";
    input.spellcheck = false;
    input.value = name;
    chip.appendChild(input);
    input.focus();
    input.select();

    let settled = false;
    const cancel = () => { if (!settled) { settled = true; buildChips(); } };
    const commit = () => {
      if (settled) return;
      settled = true;
      const v = input.value.trim();
      if (!v || v === name) { buildChips(); return; }
      call("mb_rename_profile", name, v)
        .then((err) => { if (err) chipError(chip, err); else buildChips(); },
              () => chipError(chip, "Rename failed"));
    };
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") { e.preventDefault(); commit(); }
      if (e.key === "Escape") { e.preventDefault(); cancel(); }
    });
    input.addEventListener("blur", commit);
  }

  // Replace a chip's contents with an inline "Delete 'name'? [Delete] [Cancel]".
  function startDelete(chip, name) {
    chip.onclick = null;
    chip.classList.add("confirm");
    chip.innerHTML = "";
    const label = document.createElement("span");
    label.className = "chip-name";
    label.textContent = "Delete “" + name + "”?";
    const del = document.createElement("button");
    del.className = "chip-btn danger";
    del.textContent = "Delete";
    del.onclick = (e) => {
      e.stopPropagation();
      call("mb_delete_profile", name)
        .then((err) => { if (err) chipError(chip, err); else buildChips(); },
              () => chipError(chip, "Delete failed"));
    };
    const no = document.createElement("button");
    no.className = "chip-btn";
    no.textContent = "Cancel";
    no.onclick = (e) => { e.stopPropagation(); buildChips(); };
    chip.appendChild(label);
    chip.appendChild(del);
    chip.appendChild(no);
  }

  // Briefly show an error in place of the chip, then rebuild the row.
  function chipError(chip, msg) {
    chip.className = "chip error";
    chip.onclick = null;
    chip.innerHTML = "";
    const s = document.createElement("span");
    s.className = "chip-name";
    s.textContent = msg;
    chip.appendChild(s);
    setTimeout(buildChips, 2200);
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
    // dismiss the context menu on an outside click or Escape
    document.addEventListener("pointerdown", (e) => {
      const m = el("ctx");
      if (m && !m.contains(e.target)) closeMenu();
    }, true);
    document.addEventListener("keydown", (e) => { if (e.key === "Escape") closeMenu(); });

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
