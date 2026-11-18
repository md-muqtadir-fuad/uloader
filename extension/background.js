// U-loader browser extension - background service worker.
//
// * Right-click menus: send a link / media / page to U-loader.
// * Captures browser downloads of chosen file types (like IDM) and hands
//   them to U-loader. If U-loader is not reachable the browser keeps the
//   download, so nothing is ever lost.
// * Watches network traffic for videos and streams playing in each tab and
//   lists them in the popup (badge shows how many were found).
//
// Talks to uloader.exe through native messaging (host "com.uloader.host").
// Cookies only ever go to that local program, never anywhere else.

const HOST = "com.uloader.host";

const DEFAULTS = {
  capture: true,
  connections: 8,
  quality: 0,
  exts: "zip rar 7z gz bz2 xz tar iso img exe msi apk dmg pkg deb rpm " +
        "mp4 mkv avi mov wmv flv webm m4v 3gp mp3 m4a flac wav aac ogg opus " +
        "pdf epub torrent bin",
};

async function getSettings() {
  return chrome.storage.local.get(DEFAULTS);
}

function native(msg) {
  return new Promise((resolve) => {
    try {
      chrome.runtime.sendNativeMessage(HOST, msg, (resp) => {
        if (chrome.runtime.lastError) {
          resolve({ ok: false, error: chrome.runtime.lastError.message || "not connected" });
        } else {
          resolve(resp || { ok: false, error: "no answer from U-loader" });
        }
      });
    } catch (e) {
      resolve({ ok: false, error: String(e && e.message || e) });
    }
  });
}

async function cookiesFor(url) {
  if (!/^https?:/i.test(url)) return [];
  try {
    const list = await chrome.cookies.getAll({ url });
    return list.slice(0, 500).map((c) => ({
      domain: c.domain, hostOnly: c.hostOnly, path: c.path, secure: c.secure,
      name: c.name, value: c.value, expirationDate: Math.floor(c.expirationDate || 0),
    }));
  } catch (e) {
    return [];
  }
}

// mode: "auto" (let U-loader decide), "direct" (plain file), "ytdlp" (video page)
async function sendToUloader({ url, referer = "", filename = "", mode = "auto" }) {
  if (!url || !/^(https?|ftp):|^magnet:\?/i.test(url)) {
    return { ok: false, error: "This kind of link cannot be downloaded." };
  }
  const s = await getSettings();
  return native({
    action: "download",
    url, referer, filename, mode,
    ua: navigator.userAgent,
    connections: s.connections,
    quality: s.quality,
    cookies: await cookiesFor(url),
  });
}

function notify(title, message) {
  try {
    chrome.notifications.create({
      type: "basic", iconUrl: "icons/icon128.png", title, message, priority: 1,
    });
  } catch (e) { /* notifications are optional */ }
}

function reportFailure(res) {
  const notConnected = /native messaging host|not found|forbidden|not connected/i.test(res.error || "");
  notify("U-loader",
    notConnected
      ? "U-loader is not connected to this browser. Run \"uloader --install\" once (or Settings > Connect to browsers in the U-loader window)."
      : "Could not start the download: " + (res.error || "unknown error"));
}

// ---------------------------------------------------------------------
// context menus
// ---------------------------------------------------------------------
chrome.runtime.onInstalled.addListener(() => {
  chrome.contextMenus.removeAll(() => {
    chrome.contextMenus.create({ id: "ul-link", title: "Download with U-loader", contexts: ["link"] });
    chrome.contextMenus.create({ id: "ul-media", title: "Download this media with U-loader", contexts: ["video", "audio", "image"] });
    chrome.contextMenus.create({ id: "ul-selection", title: "Download selected links with U-loader", contexts: ["selection"] });
    chrome.contextMenus.create({ id: "ul-page", title: "Download video from this page (U-loader)", contexts: ["page", "frame"] });
  });
});

const LINK_RE = /\b(?:https?|ftp):\/\/[^\s"'<>]+|magnet:\?[^\s"'<>]+/gi;

chrome.contextMenus.onClicked.addListener(async (info) => {
  const page = info.frameUrl || info.pageUrl || "";
  let results = [];
  if (info.menuItemId === "ul-link") {
    results.push(await sendToUloader({ url: info.linkUrl, referer: page }));
  } else if (info.menuItemId === "ul-media") {
    const src = info.srcUrl || "";
    // blob:/data: media cannot be fetched from outside the page: let the
    // site extractor find the real stream from the page instead.
    if (/^(blob|data):/i.test(src)) results.push(await sendToUloader({ url: page, mode: "ytdlp" }));
    else results.push(await sendToUloader({ url: src, referer: page }));
  } else if (info.menuItemId === "ul-selection") {
    const links = [...new Set((info.selectionText || "").match(LINK_RE) || [])].slice(0, 50);
    if (!links.length) { notify("U-loader", "No links found in the selected text."); return; }
    for (const u of links) results.push(await sendToUloader({ url: u, referer: page }));
  } else if (info.menuItemId === "ul-page") {
    results.push(await sendToUloader({ url: page, mode: "ytdlp" }));
  }
  const bad = results.find((r) => !r.ok);
  if (bad) reportFailure(bad);
});

// ---------------------------------------------------------------------
// capture browser downloads
// ---------------------------------------------------------------------
function extOf(name) {
  const m = /\.([a-z0-9]{1,8})$/i.exec(name || "");
  return m ? m[1].toLowerCase() : "";
}

async function maybeCapture(item) {
  const s = await getSettings();
  if (!s.capture) return false;
  const url = item.finalUrl || item.url || "";
  if (!/^(https?|ftp):/i.test(url)) return false;          // blob:, data:, file: stay in the browser
  if (item.byExtensionId) return false;                     // started by an extension
  if (item.incognito) return false;                          // never leak private sessions
  const name = (item.filename || "").split(/[\\/]/).pop();
  const wanted = new Set(String(s.exts).toLowerCase().split(/[\s,;.]+/).filter(Boolean));
  let pathExt = "";
  try { pathExt = extOf(new URL(url).pathname); } catch (e) { /* odd URL */ }
  if (!wanted.has(extOf(name)) && !wanted.has(pathExt)) return false;

  const res = await sendToUloader({ url, referer: item.referrer || "", filename: name, mode: "direct" });
  chrome.storage.session.set({ lastCapture: { url, name, ok: !!res.ok, error: res.error || "", at: Date.now() } });
  if (!res.ok) {
    reportFailure(res);
    return false;                                            // the browser keeps it
  }
  try { await chrome.downloads.cancel(item.id); } catch (e) { /* already finished */ }
  try { await chrome.downloads.erase({ id: item.id }); } catch (e) { /* ignore */ }
  return true;
}

chrome.downloads.onDeterminingFilename.addListener((item, suggest) => {
  maybeCapture(item).then((taken) => { if (!taken) suggest(); })
                    .catch(() => suggest());
  return true;   // we answer asynchronously
});

// ---------------------------------------------------------------------
// media detection (per tab)
// ---------------------------------------------------------------------
const MEDIA_EXT = /\.(m3u8|mp4|webm|mkv|mov|flv|m4v|mp3|m4a|aac|ogg|opus|wav|flac)(?:[?#]|$)/i;
const SEGMENT_EXT = /\.(ts|m4s|cmfv|cmfa)(?:[?#]|$)/i;
// Big sites stream in small pieces; the "video on this page" button handles them.
const PIECEWISE = /googlevideo\.com|\.fbcdn\.net|\.cdninstagram\.com|video\.twimg\.com\/.*\.m4s|tiktokcdn/i;

function headerMap(list) {
  const h = {};
  for (const x of list || []) h[x.name.toLowerCase()] = x.value || "";
  return h;
}

async function addMedia(tabId, entry) {
  const key = "tab_" + tabId;
  const cur = (await chrome.storage.session.get(key))[key] || [];
  if (cur.some((e) => e.url === entry.url)) return;
  cur.unshift(entry);
  if (cur.length > 40) cur.length = 40;
  await chrome.storage.session.set({ [key]: cur });
  updateBadge(tabId, cur.length);
}

function updateBadge(tabId, n) {
  chrome.action.setBadgeBackgroundColor({ tabId, color: "#2563eb" }).catch(() => {});
  chrome.action.setBadgeText({ tabId, text: n ? String(n) : "" }).catch(() => {});
}

async function clearTab(tabId) {
  await chrome.storage.session.remove("tab_" + tabId);
  updateBadge(tabId, 0);
}

chrome.webRequest.onHeadersReceived.addListener(
  (d) => {
    if (d.tabId < 0 || d.statusCode >= 400 || !/^https?:/i.test(d.url)) return;
    if (SEGMENT_EXT.test(d.url) || PIECEWISE.test(d.url)) return;
    const h = headerMap(d.responseHeaders);
    const ct = (h["content-type"] || "").toLowerCase().split(";")[0].trim();
    let kind = null;
    if (/mpegurl/.test(ct) || /\.m3u8(?:[?#]|$)/i.test(d.url)) kind = "HLS stream";
    else if (/^(video|audio)\//.test(ct) && !/mp2t|iso\.segment/.test(ct)) kind = ct;
    else if (MEDIA_EXT.test(d.url) && (ct === "application/octet-stream" || ct === "binary/octet-stream" || !ct)) kind = "media";
    if (!kind) return;
    let size = 0;
    const m = /\/(\d+)\s*$/.exec(h["content-range"] || "");
    if (m) size = Number(m[1]);
    else if (h["content-length"]) size = Number(h["content-length"]) || 0;
    if (kind !== "HLS stream" && size && size < 200 * 1024) return;   // thumbnails, previews
    addMedia(d.tabId, { url: d.url, kind, size, at: Date.now() });
  },
  { urls: ["<all_urls>"], types: ["media", "xmlhttprequest", "other", "object"] },
  ["responseHeaders"]
);

chrome.tabs.onUpdated.addListener((tabId, info) => {
  if (info.url) clearTab(tabId);     // navigated somewhere else
});
chrome.tabs.onRemoved.addListener((tabId) => { clearTab(tabId); });

// ---------------------------------------------------------------------
// popup <-> background
// ---------------------------------------------------------------------
chrome.runtime.onMessage.addListener((msg, sender, reply) => {
  (async () => {
    if (msg.type === "ping") reply(await native({ action: "ping" }));
    else if (msg.type === "open") reply(await native({ action: "open" }));
    else if (msg.type === "download") reply(await sendToUloader(msg.job));
    else if (msg.type === "media") reply((await chrome.storage.session.get("tab_" + msg.tabId))["tab_" + msg.tabId] || []);
    else if (msg.type === "media-here") {              // from the video button in a page
      const id = sender.tab ? sender.tab.id : -1;
      reply((await chrome.storage.session.get("tab_" + id))["tab_" + id] || []);
    }
    else reply({ ok: false, error: "unknown request" });
  })();
  return true;
});
