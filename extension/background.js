// U-loader browser extension - background service worker.
//
// * Right-click menus: send a link / media / page to U-loader.
// * Captures browser downloads of chosen file types (like IDM). The browser
//   copy is only paused while U-loader checks it can get the file; if it
//   can't (login walls, 403s, U-loader not installed) the browser resumes
//   its own download, so nothing is ever lost.
// * Watches network traffic for videos and streams playing in each tab and
//   lists them in the popup (badge shows how many were found).
//
// Talks to uloader.exe through native messaging (host "com.uloader.host").
// Cookies only ever go to that local program, never anywhere else.
// Incognito tabs are never recorded or captured.

const HOST = "com.uloader.host";

const DEFAULTS = {
  capture: true,
  connections: 8,
  quality: 0,
  exts: "zip rar 7z gz bz2 xz tar iso img exe msi apk dmg pkg deb rpm " +
        "mp4 mkv avi mov wmv flv webm m4v 3gp mp3 m4a flac wav aac ogg opus " +
        "pdf epub torrent bin",
};
const MODES = new Set(["auto", "direct", "ytdlp"]);

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
          resolve(resp && typeof resp === "object" ? resp : { ok: false, error: "no answer from U-loader" });
        }
      });
    } catch (e) {
      resolve({ ok: false, error: String((e && e.message) || e) });
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

function validUrl(u) {
  return typeof u === "string" && u.length < 16384 && /^(https?|ftp):\/\/|^magnet:\?/i.test(u);
}

// Video qualities a download can ask for (#3): the highest picture height,
// 0 = best, -1 = the sound only. A missing height means the next lower one.
const QUALITIES = new Set([0, 2160, 1440, 1080, 720, 480, 360, -1]);

// mode: "auto" (let U-loader decide), "direct" (plain file), "ytdlp" (video page)
// verify: ask U-loader to check it can really get the file before accepting
// quality: one of QUALITIES for this download; undefined = the popup's default
async function sendToUloader({ url, referer = "", filename = "", mode = "auto", verify = false, quality } = {}) {
  if (!validUrl(url)) return { ok: false, error: "This kind of link cannot be downloaded." };
  if (!MODES.has(mode)) mode = "auto";
  if (typeof referer !== "string" || !/^https?:/i.test(referer)) referer = "";
  if (typeof filename !== "string") filename = "";
  const s = await getSettings();
  quality = Number(quality);
  if (!QUALITIES.has(quality)) quality = QUALITIES.has(Number(s.quality)) ? Number(s.quality) : 0;
  // Automated tests switch this on from DevTools so nothing reaches a real
  // U-loader; pages can't touch extension storage.
  const t = await chrome.storage.local.get({ __uloader_test_sink: false, __uloader_test_reply: null });
  if (t.__uloader_test_sink) {
    const j = await chrome.storage.local.get({ __uloader_test_jobs: [] });
    j.__uloader_test_jobs.push({ url, referer, filename, mode, verify, quality });
    await chrome.storage.local.set({ __uloader_test_jobs: j.__uloader_test_jobs });
    return t.__uloader_test_reply || { ok: true, via: "test" };
  }
  return native({
    action: "download",
    url, referer, filename, mode, verify, quality,
    ua: navigator.userAgent,
    connections: s.connections,
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

function notConnected(res) {
  return /native messaging host|not found|forbidden|not connected/i.test((res && res.error) || "");
}

function reportFailure(res) {
  notify("U-loader",
    notConnected(res)
      ? "U-loader is not connected to this browser. Run \"uloader --install\" once (or Settings > Connect to browsers in the U-loader window)."
      : "Could not start the download: " + ((res && res.error) || "unknown error"));
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
// "see https://x.com/a.zip." -> drop the sentence punctuation after a link
const trimLink = (u) => u.replace(/[.,;:!?)\]}'"]+$/, "");

chrome.contextMenus.onClicked.addListener(async (info, tab) => {
  if (tab && tab.incognito) return;
  const page = info.frameUrl || info.pageUrl || "";
  const results = [];
  if (info.menuItemId === "ul-link") {
    results.push(await sendToUloader({ url: info.linkUrl, referer: page }));
  } else if (info.menuItemId === "ul-media") {
    const src = info.srcUrl || "";
    // blob:/data:/missing sources cannot be fetched from outside the page:
    // let the site extractor find the real stream from the page instead
    if (!validUrl(src)) results.push(await sendToUloader({ url: page, mode: "ytdlp" }));
    else results.push(await sendToUloader({ url: src, referer: page }));
  } else if (info.menuItemId === "ul-selection") {
    const links = [...new Set(((info.selectionText || "").match(LINK_RE) || []).map(trimLink))].slice(0, 50);
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

async function tryCall(fn) {
  try { await fn(); return true; } catch (e) { return false; }
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

  // Hold the browser's copy while U-loader checks it can really get the
  // file (some sites refuse anything but the browser). Never lose it.
  const paused = await tryCall(() => chrome.downloads.pause(item.id));
  const res = await sendToUloader({ url, referer: item.referrer || "", filename: name, mode: "direct", verify: true });
  chrome.storage.session.set({ lastCapture: { url, name, ok: !!res.ok, error: res.error || "", status: res.status || 0, at: Date.now() } });
  if (!res.ok) {
    if (paused) await tryCall(() => chrome.downloads.resume(item.id));
    if (notConnected(res)) reportFailure(res);
    else notify("U-loader", "U-loader couldn't download this file" +
                (res.status ? " (the server said " + res.status + ")" : res.error ? " (" + res.error + ")" : "") +
                ", so the browser kept the download.");
    return false;                                            // the browser keeps it
  }
  await tryCall(() => chrome.downloads.cancel(item.id));
  await tryCall(() => chrome.downloads.erase({ id: item.id }));
  return true;
}

chrome.downloads.onDeterminingFilename.addListener((item, suggest) => {
  let answered = false;
  const answer = () => { if (!answered) { answered = true; try { suggest(); } catch (e) { /* gone */ } } };
  maybeCapture(item).then((taken) => { if (!taken) answer(); }, answer);
  return true;   // we answer asynchronously
});

// ---------------------------------------------------------------------
// media detection (per tab)
// ---------------------------------------------------------------------
const MEDIA_EXT = /\.(m3u8|mp4|webm|mkv|mov|flv|m4v|mp3|m4a|aac|ogg|opus|wav|flac)(?:[?#]|$)/i;
const SEGMENT_EXT = /\.(ts|m4s|cmfv|cmfa)(?:[?#]|$)/i;
// Big sites stream in small pieces; the "video on this page" button handles them.
const PIECEWISE = /googlevideo\.com|\.fbcdn\.net|\.cdninstagram\.com|video\.twimg\.com\/.*\.m4s|tiktokcdn/i;
const incognitoTabs = new Set();

function headerMap(list) {
  const h = {};
  for (const x of list || []) h[x.name.toLowerCase()] = x.value || "";
  return h;
}

// storage.session updates for one tab run one after another (no lost updates)
const tabQueue = new Map();
function perTab(tabId, job) {
  const prev = tabQueue.get(tabId) || Promise.resolve();
  const next = prev.then(job, job).catch(() => {});
  tabQueue.set(tabId, next);
  next.then(() => { if (tabQueue.get(tabId) === next) tabQueue.delete(tabId); });
  return next;
}

function addMedia(tabId, entry) {
  return perTab(tabId, async () => {
    const key = "tab_" + tabId;
    const cur = (await chrome.storage.session.get(key))[key] || [];
    if (cur.some((e) => e.url === entry.url)) return;
    cur.unshift(entry);
    if (cur.length > 40) cur.length = 40;
    await chrome.storage.session.set({ [key]: cur });
    updateBadge(tabId, cur.length);
  });
}

function clearTab(tabId) {
  return perTab(tabId, async () => {
    await chrome.storage.session.remove("tab_" + tabId);
    updateBadge(tabId, 0);
  });
}

function updateBadge(tabId, n) {
  chrome.action.setBadgeBackgroundColor({ tabId, color: "#2563eb" }).catch(() => {});
  chrome.action.setBadgeText({ tabId, text: n ? String(n) : "" }).catch(() => {});
}

chrome.webRequest.onHeadersReceived.addListener(
  (d) => {
    if (d.tabId < 0 || incognitoTabs.has(d.tabId) || !/^https?:/i.test(d.url)) return;
    // real content only: no redirects, "no content" or "not modified" answers
    if (d.statusCode < 200 || d.statusCode >= 300 || d.statusCode === 204) return;
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

chrome.tabs.onUpdated.addListener((tabId, info, tab) => {
  if (tab && tab.incognito) incognitoTabs.add(tabId);
  // a new page (navigation or reload) starts with an empty list
  if (info.url || info.status === "loading") clearTab(tabId);
});
chrome.tabs.onCreated.addListener((tab) => { if (tab.incognito) incognitoTabs.add(tab.id); });
chrome.tabs.onRemoved.addListener((tabId) => { incognitoTabs.delete(tabId); clearTab(tabId); });

// ---------------------------------------------------------------------
// popup / page button <-> background
// ---------------------------------------------------------------------
// The picture heights the video on `url` is offered in, highest first (#9):
// { ok: true, heights: [1080, 720, ...] } or { ok: false, error }. U-loader
// asks yt-dlp (nothing is downloaded); answers are kept for 10 minutes.
async function qualitiesFor(url) {
  if (typeof url !== "string" || !/^https?:/i.test(url) || url.length > 16384) return { ok: false, error: "no page" };
  const key = "q:" + url;
  try {
    const c = (await chrome.storage.session.get(key))[key];
    if (c && Date.now() - c.at < 10 * 60 * 1000) return c.res;
  } catch (e) { /* no cache */ }
  // the automated tests' sink: a fixed answer, nothing reaches U-loader
  const t = await chrome.storage.local.get({ __uloader_test_sink: false, __uloader_test_heights: null });
  if (t.__uloader_test_sink) return { ok: true, heights: t.__uloader_test_heights || [1080, 720, 480, 360] };
  const res = await native({ action: "formats", url, ua: navigator.userAgent, cookies: await cookiesFor(url) });
  const out = res && res.ok && Array.isArray(res.heights)
    ? { ok: true, heights: res.heights.filter((h) => Number.isInteger(h) && h >= 100 && h <= 10000) }
    : { ok: false, error: (res && res.error) || "no answer" };
  if (out.ok) { try { await chrome.storage.session.set({ [key]: { at: Date.now(), res: out } }); } catch (e) { /* full */ } }
  return out;
}

async function mediaFor(tabId) {
  if (typeof tabId !== "number") return [];
  return (await chrome.storage.session.get("tab_" + tabId))["tab_" + tabId] || [];
}

chrome.runtime.onMessage.addListener((msg, sender, reply) => {
  (async () => {
    try {
      if (sender.id !== chrome.runtime.id || !msg || typeof msg !== "object" || typeof msg.type !== "string") {
        reply({ ok: false, error: "bad request" });
        return;
      }
      const fromPage = !!sender.tab;                    // the video button runs inside web pages
      if (fromPage) {
        if (sender.tab.incognito) { reply({ ok: false, error: "not available in private windows" }); return; }
        if (msg.type === "download") {
          const j = msg.job || {};
          reply(await sendToUloader({ url: j.url, referer: j.referer, mode: j.mode, quality: j.quality }));
        } else if (msg.type === "qualities") {
          reply(await qualitiesFor(sender.tab.url));     // only the page the button is on
        } else if (msg.type === "media-here") {
          reply(await mediaFor(sender.tab.id));
        } else {
          reply({ ok: false, error: "not allowed" });
        }
        return;
      }
      // the popup
      if (msg.type === "ping") reply(await native({ action: "ping" }));
      else if (msg.type === "open") reply(await native({ action: "open" }));
      else if (msg.type === "download") reply(await sendToUloader(msg.job || {}));
      else if (msg.type === "media") reply(await mediaFor(msg.tabId));
      else if (msg.type === "qualities") reply(await qualitiesFor(msg.url));
      else reply({ ok: false, error: "unknown request" });
    } catch (e) {
      reply({ ok: false, error: String((e && e.message) || e) });
    }
  })();
  return true;
});
