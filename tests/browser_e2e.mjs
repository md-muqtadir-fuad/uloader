// End-to-end test of the browser extension with a real Chromium browser.
//
//   node tests/browser_e2e.mjs <browser.exe> <extension dir> <server base URL> <out dir> <capture-name>
//
// Starts the browser with a throw-away profile and the extension loaded,
// then drives it over the DevTools protocol:
//   1. the extension loads with the expected fixed ID
//   2. it reaches uloader.exe over native messaging (ping only: no side effects)
//   3. "download" through the extension reaches the hand-over point
//   4. a video playing in a page is detected (badge list)
//   5. a normal browser download is taken over (U-loader says yes) ...
//   6. ... and kept by the browser when U-loader refuses it (e.g. HTTP 403)
// Every job goes to the extension's test sink (chrome.storage.local
// __uloader_test_*): nothing ever reaches a real U-loader window, and the
// browser saves into <out dir>, never the user's Downloads folder.
// The ping needs `uloader --install`. Node 22+ (global WebSocket).
import { spawn } from "node:child_process";
import { mkdtempSync, mkdirSync, rmSync, existsSync, statSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [browser, extDir, base, outDir, captureName] = process.argv.slice(2);
const EXT_ID = "ednpijjpophhlcbidanppgjliblinjga";
const port = 9300 + Math.floor(Math.random() * 500);
const profile = mkdtempSync(join(tmpdir(), "ul-browser-"));
// The browser's own downloads go to the test folder, never the user's
// Downloads. (A profile preference: the DevTools download override would
// also hide the downloads from extensions, which is what is tested here.)
mkdirSync(join(profile, "Default"), { recursive: true });
writeFileSync(join(profile, "Default", "Preferences"),
              JSON.stringify({ download: { default_directory: outDir, prompt_for_download: false, directory_upgrade: true } }));
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
], { stdio: "ignore",
  // the native host the browser starts keeps its data in the test profile, never %APPDATA%\U-loader
  env: { ...process.env, ULOADER_DATA: join(profile, "uloader-data") } });

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

  // the extension's test sink: jobs are recorded, nothing reaches a real U-loader
  await ext.eval(`chrome.storage.local.set({ __uloader_test_sink: true, __uloader_test_jobs: [],
                                             __uloader_test_reply: null, capture: true }).then(() => true)`);
  const jobsSent = () => ext.eval(`chrome.storage.local.get({ __uloader_test_jobs: [] }).then((x) => x.__uloader_test_jobs)`);
  const sinkOn = await ext.eval(`chrome.storage.local.get("__uloader_test_sink").then((x) => x.__uloader_test_sink === true)`);
  ok("test sink is on (nothing can reach a real U-loader)", sinkOn);
  if (!sinkOn) throw new Error("refusing to continue without the test sink");

  const browserWs = await new CDP(version.webSocketDebuggerUrl).open();

  const ping = await ext.eval(`new Promise(r => chrome.runtime.sendNativeMessage("com.uloader.host", {action:"ping"},
      x => r(x || {ok:false, error: chrome.runtime.lastError && chrome.runtime.lastError.message})))`);
  ok("native messaging ping", ping?.ok === true, JSON.stringify(ping));

  const link = base + "/r/three.bin";
  const job = await ext.eval(`sendToUloader({ url: ${JSON.stringify(link)}, mode: "direct" })`);
  const sent = await jobsSent();
  ok("download through the extension reaches the hand-over", job?.ok === true && sent.some((j) => j.url === link && j.mode === "direct"),
     JSON.stringify(sent));

  // media detection
  const tgt = await browserWs.send("Target.createTarget", { url: `${base}/videopage/clip.mp4` });
  const tabTarget = tgt.result?.targetId;
  const media = await waitFor(async () => {
    const all = await ext.eval(`chrome.storage.session.get(null)`);
    const lists = Object.entries(all || {}).filter(([k]) => k.startsWith("tab_")).flatMap(([, v]) => v);
    return lists.find((m) => m.url.includes("/v/clip.mp4"));
  }, 15000);
  ok("video on a page is detected", media, media ? `${media.kind}, ${media.size} bytes` : "");

  // download capture. If another download manager is hooked into the browser
  // it grabs real downloads before extensions see them: set
  // UL_E2E_DIRECT_CAPTURE=1 to feed the extension's capture handler a
  // download item directly instead.
  const direct = process.env.UL_E2E_DIRECT_CAPTURE === "1";
  const capture = async (name) => {
    await ext.eval(`chrome.storage.session.remove("lastCapture").then(() => true)`);
    if (direct) {
      await ext.eval(`maybeCapture({ id: 987654321, url: ${JSON.stringify(base + "/r/" + name)},
          finalUrl: ${JSON.stringify(base + "/r/" + name)}, filename: ${JSON.stringify(name)},
          referrer: ${JSON.stringify(base + "/videopage/clip.mp4")}, incognito: false })`);
    } else {
      // the open page goes to the file, as a click would (a new tab opened
      // through DevTools does not start a download in Edge)
      const info = await waitFor(async () => (await json("/json/list")).find((t) => t.id === tabTarget), 5000);
      const pg = await new CDP(info.webSocketDebuggerUrl).open();
      await pg.send("Page.navigate", { url: `${base}/r/${name}` });
      pg.close();
    }
    return waitFor(async () => (await ext.eval(`chrome.storage.session.get("lastCapture")`))?.lastCapture, 15000);
  };
  const note = direct ? " (handler called directly)" : "";

  // 5. U-loader takes it: the browser's copy is cancelled and erased
  const last = await capture(captureName);
  const sentCap = (await jobsSent()).find((j) => j.url.endsWith("/r/" + captureName));
  ok("extension took over the browser download" + note, last?.ok && sentCap?.verify === true,
     JSON.stringify(last) + " job=" + JSON.stringify(sentCap));
  const items = await ext.eval(`chrome.downloads.search({})`);
  ok("browser's own copy cancelled", Array.isArray(items) && !items.some((d) => d.state === "complete"),
     JSON.stringify((items || []).map((d) => d.state)));

  // 6. U-loader refuses (the server said 403 to it): the browser keeps its download
  await ext.eval(`chrome.storage.local.set({ __uloader_test_reply: { ok: false, error: "refused", status: 403 } }).then(() => true)`);
  const refused = await capture("three.bin");
  ok("refused hand-over is reported" + note, refused && refused.ok === false && refused.status === 403, JSON.stringify(refused));
  if (!direct) {
    const kept = await waitFor(() => existsSync(join(outDir, "three.bin")) && statSync(join(outDir, "three.bin")).size === 3000007, 20000);
    ok("the browser kept and finished its own download", kept);
  }
  ok("every job went to the test sink", (await jobsSent()).length >= 3);
  await ext.eval(`chrome.storage.local.set({ __uloader_test_sink: false, __uloader_test_reply: null }).then(() => true)`);
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
