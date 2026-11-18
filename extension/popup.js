// U-loader extension popup.
const $ = (id) => document.getElementById(id);

const DEFAULTS = {
  tagger: true,
  capture: true,
  connections: 8,
  quality: 0,
  exts: "zip rar 7z gz bz2 xz tar iso img exe msi apk dmg pkg deb rpm " +
        "mp4 mkv avi mov wmv flv webm m4v 3gp mp3 m4a flac wav aac ogg opus " +
        "pdf epub torrent bin",
};

let tab = null;

function ask(msg) {
  return new Promise((resolve) => {
    chrome.runtime.sendMessage(msg, (r) => {
      if (chrome.runtime.lastError) resolve({ ok: false, error: chrome.runtime.lastError.message });
      else resolve(r);
    });
  });
}

function say(text, ok) {
  const el = $("msg");
  el.textContent = text;
  el.className = "msg " + (ok ? "ok" : "bad");
}

function fmtSize(n) {
  if (!n) return "";
  const u = ["B", "KB", "MB", "GB", "TB"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) { n /= 1024; i++; }
  return (i ? n.toFixed(n < 10 ? 1 : 0) : n) + " " + u[i];
}

function nameOf(url) {
  try {
    const p = new URL(url).pathname.split("/").filter(Boolean);
    return decodeURIComponent(p[p.length - 1] || new URL(url).hostname);
  } catch (e) {
    return url;
  }
}

async function send(job, button) {
  if (button) button.disabled = true;
  const r = await ask({ type: "download", job });
  if (button) button.disabled = false;
  if (r && r.ok) say("Sent to U-loader.", true);
  else say("Failed: " + ((r && r.error) || "U-loader is not reachable"), false);
  return r;
}

async function checkConnection() {
  const r = await ask({ type: "ping" });
  const st = $("status");
  if (r && r.ok) {
    st.textContent = "connected";
    st.className = "status ok";
    $("version").textContent = "U-loader " + r.version + (r.ytdlp ? "" : " · video sites need yt-dlp");
    $("setup").hidden = true;
  } else {
    st.textContent = "not connected";
    st.className = "status bad";
    $("setup").hidden = false;
  }
}

async function showMedia() {
  const list = $("media");
  list.textContent = "";
  if (!tab) return;
  const items = await ask({ type: "media", tabId: tab.id });
  $("mediaEmpty").hidden = Array.isArray(items) && items.length > 0;
  for (const it of items || []) {
    const li = document.createElement("li");
    const info = document.createElement("div");
    info.className = "info";
    const name = document.createElement("div");
    name.className = "name";
    name.textContent = nameOf(it.url);
    name.title = it.url;
    const meta = document.createElement("div");
    meta.className = "meta";
    meta.textContent = [it.kind, fmtSize(it.size)].filter(Boolean).join(" · ");
    info.append(name, meta);
    const b = document.createElement("button");
    b.type = "button";
    b.textContent = "Download";
    b.addEventListener("click", () => send({ url: it.url, referer: tab.url, mode: "direct" }, b));
    li.append(info, b);
    list.append(li);
  }
}

async function loadSettings() {
  const s = await chrome.storage.local.get(DEFAULTS);
  $("tagger").checked = !!s.tagger;
  $("capture").checked = !!s.capture;
  $("exts").value = s.exts;
  $("connections").value = String(s.connections);
  $("quality").value = String(s.quality);
}

function saveSettings() {
  chrome.storage.local.set({
    tagger: $("tagger").checked,
    capture: $("capture").checked,
    exts: $("exts").value.trim() || DEFAULTS.exts,
    connections: Number($("connections").value) || 8,
    quality: Number($("quality").value) || 0,
  });
}

document.addEventListener("DOMContentLoaded", async () => {
  [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
  const pageOk = tab && /^https?:/i.test(tab.url || "");
  $("pageVideo").disabled = !pageOk;
  $("pageVideo").addEventListener("click", (e) => send({ url: tab.url, mode: "ytdlp" }, e.currentTarget));

  $("linkForm").addEventListener("submit", (e) => {
    e.preventDefault();
    const v = $("link").value.trim();
    if (!v) return;
    send({ url: /^(https?|ftp):|^magnet:/i.test(v) ? v : "https://" + v, referer: pageOk ? tab.url : "" })
      .then((r) => { if (r && r.ok) $("link").value = ""; });
  });

  $("copyCmd").addEventListener("click", () => {
    navigator.clipboard.writeText("uloader --install").then(() => say("Copied.", true));
  });
  $("openApp").addEventListener("click", async () => {
    const r = await ask({ type: "open" });
    if (!r || !r.ok) say((r && r.error) || "U-loader is not reachable", false);
  });

  for (const id of ["tagger", "capture", "exts", "connections", "quality"]) $(id).addEventListener("change", saveSettings);

  await loadSettings();
  checkConnection();
  showMedia();
});
