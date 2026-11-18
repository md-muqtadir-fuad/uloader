// U-loader video tagger: a "Download this video" button that pops up on
// videos (like IDM's download panel). Runs in every page and frame.
(() => {
  if (window.__uloaderTagger) return;
  window.__uloaderTagger = true;

  const MIN_W = 220, MIN_H = 124;          // ignore thumbnails and tiny previews
  const HIDE_AFTER_MS = 3000;              // after the mouse leaves the video
  let enabled = true;
  let current = null;                      // the video the button is showing for
  let hideTimer = 0, trackTimer = 0, lastMove = 0;
  const dismissed = new WeakSet();

  chrome.storage.local.get({ tagger: true }, (s) => { enabled = !!s.tagger; });
  chrome.storage.onChanged.addListener((c) => {
    if (c.tagger) { enabled = !!c.tagger.newValue; if (!enabled) hide(); }
  });

  // ---- the button, isolated from the page in a shadow root ---------------
  const host = document.createElement("uloader-tagger");
  host.style.cssText = "all:initial;position:fixed;z-index:2147483647;top:0;left:0;display:none;";
  const root = host.attachShadow({ mode: "open" });
  root.innerHTML = `
<style>
  :host { all: initial; }
  .wrap { position: absolute; top: 0; left: 0; transform: translateX(-100%);
          display: flex; flex-direction: column; align-items: flex-end;
          font: 13px/1.3 "Segoe UI", system-ui, sans-serif; color: #fff; user-select: none; }
  .bar { display: inline-flex; align-items: stretch; border-radius: 8px; overflow: hidden;
         box-shadow: 0 2px 10px rgba(0,0,0,.45); background: rgba(20,24,32,.88);
         backdrop-filter: blur(4px); animation: in .15s ease-out; }
  @keyframes in { from { opacity: 0; transform: translateY(-4px); } to { opacity: 1; transform: none; } }
  button { all: unset; cursor: pointer; display: flex; align-items: center; gap: 7px; }
  .go { padding: 7px 12px 7px 10px; background: #2563eb; font-weight: 600; }
  .go:hover { background: #1d4ed8; }
  .go svg { width: 16px; height: 16px; flex: none; }
  .x { padding: 0 9px; color: #cbd5e1; font-size: 15px; }
  .x:hover { color: #fff; background: rgba(255,255,255,.12); }
  .menu { margin-top: 6px; min-width: 280px; max-width: 420px; border-radius: 8px; overflow: hidden;
          background: rgba(20,24,32,.96); box-shadow: 0 6px 18px rgba(0,0,0,.5); }
  .item { display: block; width: 100%; box-sizing: border-box; padding: 8px 12px; border-top: 1px solid rgba(255,255,255,.08); }
  .item:first-child { border-top: 0; }
  .item:hover { background: rgba(37,99,235,.55); }
  .label { font-weight: 600; }
  .sub { color: #a5b4c8; font-size: 11.5px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; max-width: 390px; }
  .toast { margin-top: 6px; padding: 7px 12px; border-radius: 8px; background: rgba(20,24,32,.94); }
  .toast.bad { background: rgba(160,30,40,.95); }
  [hidden] { display: none !important; }
</style>
<div class="wrap">
  <div class="bar">
    <button class="go" title="Download this video with U-loader">
      <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M12 4v11"/><path d="M7 11l5 5 5-5"/><path d="M5 20h14"/></svg>
      <span>Download this video</span>
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
    const r = v.getBoundingClientRect();
    return r.width >= MIN_W && r.height >= MIN_H && r.bottom > 20 && r.right > 20 &&
           r.top < innerHeight - 20 && r.left < innerWidth - 20 &&
           getComputedStyle(v).visibility !== "hidden";
  }

  function place() {
    if (!current) return;
    if (!usable(current)) { hide(); return; }
    const r = current.getBoundingClientRect();
    host.style.top = Math.max(4, r.top + 10) + "px";
    host.style.left = Math.min(innerWidth - 4, r.right - 10) + "px";
  }

  function show(v) {
    if (!enabled || !usable(v)) return;
    if (current !== v) { menu.hidden = true; toast.hidden = true; }
    current = v;
    attach();
    host.style.display = "block";
    place();
    clearInterval(trackTimer);
    trackTimer = setInterval(place, 250);   // players move and resize
    scheduleHide(HIDE_AFTER_MS + 2000);
  }

  function hide() {
    host.style.display = "none";
    menu.hidden = true;
    toast.hidden = true;
    clearInterval(trackTimer);
    clearTimeout(hideTimer);
    current = null;
  }

  function scheduleHide(ms) {
    clearTimeout(hideTimer);
    hideTimer = setTimeout(() => {
      if (!menu.hidden || host.matches(":hover")) { scheduleHide(1500); return; }
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

  // ---- when to show --------------------------------------------------------
  document.addEventListener("mousemove", (e) => {
    const now = performance.now();
    if (now - lastMove < 120) return;
    lastMove = now;
    if (!enabled) return;
    const v = videoAt(e.clientX, e.clientY);
    if (v) show(v);
    else if (current && !host.matches(":hover")) scheduleHide(HIDE_AFTER_MS);
  }, { passive: true, capture: true });

  document.addEventListener("play", (e) => {
    if (e.target instanceof HTMLVideoElement) show(e.target);
  }, true);

  document.addEventListener("fullscreenchange", () => { if (current) { attach(); place(); } });
  addEventListener("scroll", place, { passive: true, capture: true });
  addEventListener("resize", place, { passive: true });

  root.querySelector(".x").addEventListener("click", (e) => {
    e.stopPropagation();
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
    for (const m of seen.slice(0, 6)) {
      if (m.url === src) continue;
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
    scheduleHide(bad ? 8000 : 3500);
  }

  async function send(job) {
    menu.hidden = true;
    say("Sending to U-loader…");
    const r = await ask({ type: "download", job });
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
    if (!menu.hidden) { menu.hidden = true; return; }
    const opts = await options();
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
      b.addEventListener("click", (ev) => { ev.stopPropagation(); send(o.job); });
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
