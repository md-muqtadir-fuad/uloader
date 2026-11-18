// End-to-end test of the browser extension with a real Chromium browser.
//
//   node tests/browser_e2e.mjs <browser.exe> <extension dir> <server base URL> <out dir> <capture-name>
//
// Starts the browser with a throw-away profile and the extension loaded,
// then drives it over the DevTools protocol:
//   1. the extension loads with the expected fixed ID
//   2. it reaches uloader.exe over native messaging (ping)
//   3. "download" through the extension -> the file arrives (dir = out dir)
//   4. a video playing in a page is detected (badge list)
//   5. a normal browser download of a .zip is taken over by U-loader
// Requires `uloader --install` to have been run. Node 22+ (global WebSocket).
import { spawn, execSync } from "node:child_process";
import { mkdtempSync, rmSync, existsSync, statSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [browser, extDir, base, outDir, captureName] = process.argv.slice(2);
const EXT_ID = "ednpijjpophhlcbidanppgjliblinjga";
const port = 9300 + Math.floor(Math.random() * 500);
const profile = mkdtempSync(join(tmpdir(), "ul-browser-"));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const results = [];
const ok = (name, cond, info = "") => { results.push([name, !!cond, info]); console.log(`${cond ? "PASS" : "FAIL"}  ${name}${info ? "  " + info : ""}`); };

// A real (off-screen) window: headless browsers skip the normal download
// flow that the extension hooks into.
const proc = spawn(browser, [
  "--window-position=-2400,0", "--window-size=1000,700", `--user-data-dir=${profile}`, `--remote-debugging-port=${port}`,
  `--load-extension=${extDir}`, `--disable-extensions-except=${extDir}`,
  "--disable-features=DisableLoadExtensionCommandLineSwitch",
  "--no-first-run", "--no-default-browser-check", "--autoplay-policy=no-user-gesture-required",
  "about:blank",
], { stdio: "ignore" });

async function json(path) {
  const r = await fetch(`http://127.0.0.1:${port}${path}`);
  return r.json();
}

class CDP {
  constructor(url) { this.url = url; this.id = 0; this.pending = new Map(); }
  open() {
    return new Promise((resolve, reject) => {
      this.ws = new WebSocket(this.url);
      this.ws.onopen = () => resolve(this);
      this.ws.onerror = (e) => reject(e);
      this.ws.onmessage = (m) => {
        const msg = JSON.parse(m.data);
        if (msg.id && this.pending.has(msg.id)) { this.pending.get(msg.id)(msg); this.pending.delete(msg.id); }
      };
    });
  }
  send(method, params = {}) {
    const id = ++this.id;
    this.ws.send(JSON.stringify({ id, method, params }));
    return new Promise((r) => this.pending.set(id, r));
  }
  async eval(expr, ms = 20000) {
    const r = await Promise.race([
      this.send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true }),
      sleep(ms).then(() => { throw new Error("timed out: " + expr.slice(0, 80)); }),
    ]);
    if (r.result?.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails).slice(0, 500));
    return r.result?.result?.value;
  }
  close() { try { this.ws.close(); } catch {} }
}

async function waitFor(fn, ms, step = 250) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    try { const v = await fn(); if (v) return v; } catch {}
    await sleep(step);
  }
  return null;
}

const watchdog = setTimeout(() => { ok("whole test finished in time", false, "watchdog fired after 150 s"); proc.kill(); process.exit(1); }, 150000);
try {
  const version = await waitFor(() => json("/json/version"), 20000);
  ok("browser started", version, version?.Browser);

  const sw = await waitFor(async () => (await json("/json/list")).find(
    (t) => t.type === "service_worker" && t.url.startsWith(`chrome-extension://${EXT_ID}/`)), 20000);
  ok("extension loaded with fixed ID", sw, sw ? sw.url : "service worker not found (is --load-extension allowed?)");
  if (!sw) throw new Error("stop");
  let ext = await new CDP(sw.webSocketDebuggerUrl).open();
  await ext.send("Runtime.runIfWaitingForDebugger");
  try {
    await ext.eval("1 + 1", 5000);
  } catch (e) {
    // the worker may have been suspended: find it again
    ext.close();
    const again = await waitFor(async () => (await json("/json/list")).find(
      (t) => t.type === "service_worker" && t.url.startsWith(`chrome-extension://${EXT_ID}/`)), 15000);
    ext = await new CDP(again.webSocketDebuggerUrl).open();
    await ext.send("Runtime.runIfWaitingForDebugger");
    await ext.eval("1 + 1", 5000);
  }

  const ping = await ext.eval(`new Promise(r => chrome.runtime.sendNativeMessage("com.uloader.host", {action:"ping"},
      x => r(x || {ok:false, error: chrome.runtime.lastError && chrome.runtime.lastError.message})))`);
  ok("native messaging ping", ping?.ok === true, JSON.stringify(ping));

  const job = await ext.eval(`new Promise(r => chrome.runtime.sendNativeMessage("com.uloader.host",
      {action:"download", url:${JSON.stringify(base + "/r/three.bin")}, dir:${JSON.stringify(outDir)},
       mode:"direct", ua:navigator.userAgent, cookies:[{domain:"127.0.0.1", hostOnly:true, path:"/",
       secure:false, name:"session", value:"abc", expirationDate:0}]},
      x => r(x || {ok:false, error: chrome.runtime.lastError && chrome.runtime.lastError.message})))`);
  ok("download request accepted", job?.ok === true, JSON.stringify(job));
  const got = await waitFor(() => existsSync(join(outDir, "three.bin")) && statSync(join(outDir, "three.bin")).size === 3000007, 30000);
  ok("file downloaded through the extension", got);

  // media detection
  const browserWs = await new CDP(version.webSocketDebuggerUrl).open();
  const tgt = await browserWs.send("Target.createTarget", { url: `${base}/videopage/clip.mp4` });
  const tabTarget = tgt.result?.targetId;
  const media = await waitFor(async () => {
    const all = await ext.eval(`chrome.storage.session.get(null)`);
    const lists = Object.values(all || {}).flat();
    return lists.find((m) => m.url.includes("/v/clip.mp4"));
  }, 15000);
  ok("video on a page is detected", media, media ? `${media.kind}, ${media.size} bytes` : "");

  // download capture. If another download manager (e.g. IDM) is hooked into
  // the browser it grabs real downloads before extensions see them, so then
  // feed the extension's capture handler a download item directly.
  const idm = execSync('tasklist /FI "IMAGENAME eq IDMan.exe" /NH').toString().includes("IDMan.exe");
  if (idm) {
    await ext.eval(`maybeCapture({ id: 987654321, url: ${JSON.stringify(base + "/r/" + captureName)},
        finalUrl: ${JSON.stringify(base + "/r/" + captureName)}, filename: ${JSON.stringify(captureName)},
        referrer: ${JSON.stringify(base + "/videopage/clip.mp4")}, incognito: false })`);
  } else {
    await browserWs.send("Target.createTarget", { url: `${base}/r/${captureName}` });
  }
  const last = await waitFor(async () => (await ext.eval(`chrome.storage.session.get("lastCapture")`))?.lastCapture, 15000);
  const seen = await ext.eval(`chrome.downloads.search({}).then(l => l.map(d => ({state: d.state, danger: d.danger, error: d.error, file: d.filename})))`);
  ok("extension took over the browser download" + (idm ? " (IDM running: handler called directly)" : ""),
     last?.ok, JSON.stringify(last) + " downloads=" + JSON.stringify(seen));
  const userDl = join(process.env.USERPROFILE, "Downloads", captureName);
  const captured = await waitFor(() => existsSync(userDl) && statSync(userDl).size > 0, 30000);
  ok("captured file saved by U-loader", captured, captured ? userDl : "");
  const items = await ext.eval(`chrome.downloads.search({})`);
  ok("browser's own copy cancelled", Array.isArray(items) && !items.some((d) => d.state === "complete"),
     JSON.stringify((items || []).map((d) => d.state)));
  browserWs.close();
  ext.close();
} catch (e) {
  if (e.message !== "stop") ok("unexpected error", false, String(e.stack || e));
} finally {
  clearTimeout(watchdog);
  proc.kill();
  await sleep(1500);
  try { rmSync(profile, { recursive: true, force: true }); } catch {}
  const failed = results.filter((r) => !r[1]).length;
  console.log(`\n${results.length - failed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
}
