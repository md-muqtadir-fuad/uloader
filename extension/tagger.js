// U-loader video tagger: a small "U-loader" button that pops up on videos
// (like IDM's download panel). Runs in every page and frame.
//
// Security: the button lives in a CLOSED shadow root, so page scripts can't
// reach or click it, and every action requires a trusted (real user /
// real browser) event. A page can never start a download by itself.
(() => {
  if (window.__uloaderTagger) return;
  window.__uloaderTagger = true;

  const MIN_W = 200, MIN_H = 112;          // ignore thumbnails smaller than this
  const ICON_ONLY_BELOW = 300;             // narrow videos get just the icon
  const HIDE_AFTER_MS = 2500;              // after the mouse leaves the video
  let enabled = false;                     // stays off until the setting is read
  let current = null;                      // the video the button is showing for
  let hideTimer = 0, trackTimer = 0, lastMove = 0, busy = false;
  const dismissed = new WeakSet();

  chrome.storage.local.get({ tagger: true }, (s) => { enabled = !!s.tagger; });
  chrome.storage.onChanged.addListener((c) => {
    if (c.tagger) { enabled = !!c.tagger.newValue; if (!enabled) hide(); }
  });

  // ---- the button, sealed off from the page ------------------------------
  const host = document.createElement("uloader-tagger");
  host.style.cssText = "all:initial;position:fixed;z-index:2147483647;top:0;left:0;display:none;";
  const root = host.attachShadow({ mode: "closed" });
  root.innerHTML = `
<style>
  :host { all: initial; }
  /* width: max-content + nowrap: next to the right edge of the window the
     box got squeezed and "U-loader" broke after the hyphen onto two lines */
  .wrap { position: absolute; top: 0; left: 0; transform: translateX(-100%); width: max-content;
          display: flex; flex-direction: column; align-items: flex-end; white-space: nowrap;
          font: 12px/1.2 "Segoe UI", system-ui, sans-serif; color: #fff; user-select: none; }
  .bar { display: inline-flex; flex: none; align-items: stretch; height: 24px; border-radius: 6px; overflow: hidden;
         background: rgba(15,18,25,.72); box-shadow: 0 1px 6px rgba(0,0,0,.35);
         opacity: .9; transition: opacity .12s; animation: in .12s ease-out; }
  .bar:hover { opacity: 1; }
  @keyframes in { from { opacity: 0; } to { opacity: .9; } }
  button { all: unset; cursor: pointer; display: flex; align-items: center; }
  .go { gap: 5px; padding: 0 8px 0 7px; background: #2563eb; font-weight: 600; letter-spacing: .1px; }
  .go:hover { background: #1d4ed8; }
  .go svg { width: 13px; height: 13px; flex: none; }
  .small .go { padding: 0 6px; }
  .small .go span { display: none; }
  .x { padding: 0 6px; color: #cbd5e1; font-size: 13px; }
  .x:hover { color: #fff; background: rgba(255,255,255,.14); }
  .menu { margin-top: 4px; min-width: 210px; max-width: 360px; border-radius: 6px; overflow: hidden;
          background: rgba(15,18,25,.96); box-shadow: 0 4px 14px rgba(0,0,0,.5); }
  .item { display: block; width: 100%; box-sizing: border-box; padding: 6px 10px; border-top: 1px solid rgba(255,255,255,.08); }
  .item:first-child { border-top: 0; }
  .item:hover { background: rgba(37,99,235,.6); }
  .label { font-weight: 600; }
  .sub { color: #a5b4c8; font-size: 11px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 340px; }
  .toast { margin-top: 4px; padding: 5px 9px; border-radius: 6px; background: rgba(15,18,25,.94); max-width: 300px; }
  .toast.bad { background: rgba(160,30,40,.95); }
  [hidden] { display: none !important; }
</style>
<div class="wrap">
  <div class="bar">
    <button class="go" title="Download this video with U-loader">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M12 4v11"/><path d="M7 11l5 5 5-5"/><path d="M5 20h14"/></svg>
      <span>U-loader</span>
    </button>
    <button class="x" title="Hide for this video">&times;</button>
  </div>
  <div class="menu" hidden></div>
  <div class="toast" hidden></div>
</div>`;
  const wrap = root.querySelector(".wrap");
  const menu = root.querySelector(".menu");
  const toast = root.querySelector(".toast");

  function attach() {
    // in fullscreen only the fullscreen element's subtree is visible
    const parent = document.fullscreenElement || document.documentElement;
    if (parent && host.parentNode !== parent) parent.appendChild(host);
  }

  function usable(v) {
    if (!v || !v.isConnected || dismissed.has(v)) return false;
    // a fullscreen <video> paints none of its children: nothing can be shown over it
    if (document.fullscreenElement && document.fullscreenElement === v) return false;
    const r = v.getBoundingClientRect();
    return r.width >= MIN_W && r.height >= MIN_H && r.bottom > 20 && r.right > 20 &&
           r.top < innerHeight - 20 && r.left < innerWidth - 20 &&
           getComputedStyle(v).visibility !== "hidden" && getComputedStyle(v).opacity !== "0";
  }

  function place() {
    if (!current) return;
    if (!usable(current)) { hide(); return; }
    const r = current.getBoundingClientRect();
    wrap.classList.toggle("small", r.width < ICON_ONLY_BELOW);
    host.style.top = Math.max(4, r.top + 8) + "px";
    host.style.left = Math.min(innerWidth - 4, r.right - 8) + "px";
  }

  function show(v) {
    if (!enabled || !usable(v)) return;
    if (current !== v) closeMenu();
    current = v;
    attach();
    host.style.display = "block";
    place();
    clearInterval(trackTimer);
    trackTimer = setInterval(place, 250);   // players move and resize
    scheduleHide(HIDE_AFTER_MS + 2000);
  }

  function closeMenu() {
    menu.hidden = true;
    toast.hidden = true;
  }

  function hide() {
    host.style.display = "none";
    closeMenu();
    clearInterval(trackTimer);
    clearTimeout(hideTimer);
    current = null;
  }

  function scheduleHide(ms) {
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => {
      if (host.matches(":hover")) { scheduleHide(1500); return; }
      hide();
    }, ms);
  }

  function videoAt(x, y) {
    for (const v of document.querySelectorAll("video")) {
      const r = v.getBoundingClientRect();
      if (x >= r.left && x <= r.right && y >= r.top && y <= r.bottom && usable(v)) return v;
    }
    return null;
  }

  // ---- when to show (only real user / browser events) -----------------------
  document.addEventListener("mousemove", (e) => {
    if (!e.isTrusted || !enabled) return;
    const now = performance.now();
    if (now - lastMove < 120) return;
    lastMove = now;
    const v = videoAt(e.clientX, e.clientY);
    if (v) show(v);
    else if (current && !host.matches(":hover")) scheduleHide(menu.hidden ? HIDE_AFTER_MS : 6000);
  }, { passive: true, capture: true });

  document.addEventListener("play", (e) => {
    if (e.isTrusted && e.target instanceof HTMLVideoElement) show(e.target);
  }, true);

  // a click anywhere else closes the menu
  document.addEventListener("mousedown", (e) => {
    if (e.isTrusted && !menu.hidden && !e.composedPath().includes(host)) closeMenu();
  }, true);

  document.addEventListener("fullscreenchange", () => { if (current) { attach(); place(); } });
  addEventListener("scroll", place, { passive: true, capture: true });
  addEventListener("resize", place, { passive: true });

  root.querySelector(".x").addEventListener("click", (e) => {
    e.stopPropagation();
    if (!e.isTrusted) return;
    if (current) dismissed.add(current);
    hide();
  });

  // ---- what to download ----------------------------------------------------
  function ask(msg) {
    return new Promise((resolve) => {
      try {
        chrome.runtime.sendMessage(msg, (r) => resolve(chrome.runtime.lastError ? null : r));
      } catch (e) { resolve(null); }   // extension was reloaded
    });
  }

  function fmtSize(n) {
    if (!n) return "";
    const u = ["B", "KB", "MB", "GB"];
    let i = 0;
    while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
    return (i ? n.toFixed(n < 10 ? 1 : 0) : n) + " " + u[i];
  }

  function nameOf(url) {
    try {
      const p = new URL(url).pathname.split("/").filter(Boolean);
      return decodeURIComponent(p[p.length - 1] || new URL(url).hostname);
    } catch (e) { return url; }
  }

  async function options() {
    const v = current;
    const src = (v && (v.currentSrc || v.src)) || "";
    const page = location.href;
    const opts = [];
    // a plain video file the page plays directly
    if (/^https?:/i.test(src) && !/\.(m4s|ts)(?:[?#]|$)/i.test(src) && !/googlevideo\.com/i.test(src)) {
      opts.push({ label: "This video file", sub: nameOf(src), job: { url: src, referer: page, mode: "direct" } });
    }
    // streams and files seen in this tab's traffic
    const seen = (await ask({ type: "media-here" })) || [];
    for (const m of Array.isArray(seen) ? seen.slice(0, 6) : []) {
      if (!m || typeof m.url !== "string" || m.url === src) continue;
      const label = m.kind === "HLS stream" ? "Stream (HLS)" : "Media file";
      opts.push({ label, sub: [nameOf(m.url), fmtSize(m.size)].filter(Boolean).join(" · "),
                  job: { url: m.url, referer: page, mode: "direct" } });
    }
    // the page itself: yt-dlp finds the best quality (YouTube & co.)
    if (/^https?:/i.test(page)) {
      opts.push({ label: "Best quality from this page", sub: location.hostname + " · via yt-dlp",
                  job: { url: page, mode: "ytdlp" } });
    }
    return opts;
  }

  function say(text, bad) {
    toast.textContent = text;
    toast.className = "toast" + (bad ? " bad" : "");
    toast.hidden = false;
    scheduleHide(bad ? 8000 : 3000);
  }

  async function send(job) {
    if (busy) return;
    busy = true;
    menu.hidden = true;
    say("Sending to U-loader…");
    const r = await ask({ type: "download", job });
    busy = false;
    if (r && r.ok) say("✓ Sent to U-loader");
    else {
      const err = (r && r.error) || "U-loader is not reachable";
      say(/native messaging|not found|forbidden|not connected/i.test(err)
            ? "U-loader is not connected - run \"uloader --install\" once" : "Failed: " + err, true);
    }
  }

  root.querySelector(".go").addEventListener("click", async (e) => {
    e.stopPropagation();
    e.preventDefault();
    if (!e.isTrusted || busy) return;
    if (!menu.hidden) { menu.hidden = true; return; }
    const forVideo = current;
    const opts = await options();
    if (current !== forVideo || host.style.display === "none") return;   // hidden meanwhile
    if (!opts.length) { say("Nothing downloadable found here", true); return; }
    if (opts.length === 1) { send(opts[0].job); return; }
    menu.textContent = "";
    for (const o of opts) {
      const b = document.createElement("button");
      b.className = "item";
      const l = document.createElement("div");
      l.className = "label";
      l.textContent = o.label;
      const s = document.createElement("div");
      s.className = "sub";
      s.textContent = o.sub || "";
      b.append(l, s);
      b.addEventListener("click", (ev) => {
        ev.stopPropagation();
        if (ev.isTrusted) send(o.job);
      });
      menu.append(b);
    }
    toast.hidden = true;
    menu.hidden = false;
    scheduleHide(10000);
  });

  // keep clicks on the button from reaching the player underneath
  for (const t of ["mousedown", "mouseup", "pointerdown", "pointerup", "dblclick"])
    host.addEventListener(t, (e) => e.stopPropagation());
})();
