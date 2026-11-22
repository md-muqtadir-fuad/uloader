// Real-browser test of the video button (extension/tagger.js).
//
//   node tests/tagger_e2e.mjs <browser.exe> <extension dir> <server base URL> <screenshot.png>
//
// Uses tests/server.py's /tagpage/<file>?w=..: a video with a transparent
// overlay on top (like YouTube's controls). The button lives in a CLOSED
// shadow root, so the test reads it through the DevTools DOM domain (which
// can see closed roots) and clicks it with real (trusted) input events.
// Sending to U-loader is intercepted inside the extension: nothing is downloaded.
import { spawn } from "node:child_process";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [browser, extDir, base, shot] = process.argv.slice(2);
const EXT_ID = "ednpijjpophhlcbidanppgjliblinjga";
const port = 9400 + Math.floor(Math.random() * 400);
const profile = mkdtempSync(join(tmpdir(), "ul-tagger-"));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const results = [];
const ok = (n, c, i = "") => { results.push(!!c); console.log(`${c ? "PASS" : "FAIL"}  ${n}${i ? "  " + i : ""}`); };

const proc = spawn(browser, ["--window-position=-2400,0", "--window-size=1100,800", `--user-data-dir=${profile}`,
  `--remote-debugging-port=${port}`, `--load-extension=${extDir}`, `--disable-extensions-except=${extDir}`,
  "--disable-features=DisableLoadExtensionCommandLineSwitch", "--no-first-run", "--no-default-browser-check",
  "about:blank"], { stdio: "ignore" });

const json = async (p) => (await fetch(`http://127.0.0.1:${port}${p}`)).json();
async function waitFor(fn, ms, step = 200) {
  const end = Date.now() + ms;
  while (Date.now() < end) { try { const v = await fn(); if (v) return v; } catch {} await sleep(step); }
  return null;
}
function cdp(url) {
  const ws = new WebSocket(url); let id = 0; const pend = new Map();
  ws.onmessage = (m) => { const x = JSON.parse(m.data); if (x.id && pend.has(x.id)) { pend.get(x.id)(x); pend.delete(x.id); } };
  return new Promise((res, rej) => {
    ws.onerror = rej;
    ws.onopen = () => res({
      send: (method, params = {}) => new Promise((r) => { const i = ++id; pend.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); }),
      async eval(expr) {
        const r = await this.send("Runtime.evaluate", { expression: expr, awaitPromise: true, returnByValue: true });
        if (r.result?.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails).slice(0, 300));
        return r.result?.result?.value;
      },
      close: () => ws.close(),
    });
  });
}

// ---- reading the closed shadow root through the DOM domain ----------------
const attr = (n, name) => { const a = n.attributes || []; for (let i = 0; i < a.length; i += 2) if (a[i] === name) return a[i + 1]; return null; };
function walk(n, pred) {
  if (!n) return null;
  if (pred(n)) return n;
  for (const c of [...(n.children || []), ...(n.shadowRoots || [])]) { const r = walk(c, pred); if (r) return r; }
  return null;
}
async function tagger(page) {
  const doc = (await page.send("DOM.getDocument", { depth: -1, pierce: true })).result.root;
  const host = walk(doc, (n) => n.localName === "uloader-tagger");
  if (!host) return null;
  const sr = (host.shadowRoots || [])[0];
  const byClass = (c) => walk(sr, (n) => (attr(n, "class") || "").split(/\s+/).includes(c));
  return { host, sr, go: byClass("go"), x: byClass("x"), menu: byClass("menu"), toast: byClass("toast"),
           wrap: byClass("wrap"), items: () => { const out = []; (function w(n) { if (!n) return; if ((attr(n, "class") || "") === "item") out.push(n); (n.children || []).forEach(w); })(byClass("menu")); return out; } };
}
async function box(page, node) {
  const r = await page.send("DOM.getBoxModel", { nodeId: node.nodeId });
  const q = r.result?.model?.border;
  if (!q) return null;
  return { x: q[0], y: q[1], w: q[2] - q[0], h: q[5] - q[1] };
}
const textOf = (n) => { let s = ""; (function w(m) { if (!m) return; if (m.nodeType === 3) s += m.nodeValue; (m.children || []).forEach(w); })(n); return s.trim(); };

const watchdog = setTimeout(() => { console.log("FAIL  watchdog: test took over 150 s"); proc.kill(); process.exit(1); }, 150000);
try {
  const version = await waitFor(() => json("/json/version"), 20000);
  const sw = await waitFor(async () => (await json("/json/list")).find(
    (t) => t.type === "service_worker" && t.url.startsWith(`chrome-extension://${EXT_ID}/`)), 20000);
  ok("extension loaded", sw);
  const ext = await cdp(sw.webSocketDebuggerUrl);
  await ext.send("Runtime.runIfWaitingForDebugger");
  ok("extension worker ready", await waitFor(() => ext.eval(`typeof chrome !== "undefined" && !!chrome.storage`), 10000));
  // the extension's test sink: jobs are recorded, nothing reaches a real U-loader
  // (kept in extension storage, so it survives service-worker restarts)
  await ext.eval(`chrome.storage.local.set({ __uloader_test_sink: true, __uloader_test_jobs: [] }).then(() => true)`);
  const jobsSent = () => ext.eval(`chrome.storage.local.get({ __uloader_test_jobs: [] }).then((x) => x.__uloader_test_jobs)`);
  const sinkOn = await ext.eval(`chrome.storage.local.get("__uloader_test_sink").then((x) => x.__uloader_test_sink === true)`);
  ok("test sink is on (nothing can reach a real U-loader)", sinkOn);
  if (!sinkOn) throw new Error("refusing to continue without the test sink");

  const b = await cdp(version.webSocketDebuggerUrl);
  async function openPage(path) {
    const tgt = (await b.send("Target.createTarget", { url: `${base}${path}` })).result.targetId;
    const info = await waitFor(async () => (await json("/json/list")).find((t) => t.id === tgt), 10000);
    const page = await cdp(info.webSocketDebuggerUrl);
    await page.send("Page.bringToFront");
    await page.send("DOM.enable");
    await waitFor(() => page.eval(`document.readyState === "complete"`), 10000);
    await sleep(900);
    return page;
  }
  const mouse = (page, type, x, y, extra = {}) => page.send("Input.dispatchMouseEvent", { type, x, y, button: "none", ...extra });
  const click = async (page, x, y) => {
    await mouse(page, "mouseMoved", x, y);
    await mouse(page, "mousePressed", x, y, { button: "left", clickCount: 1 });
    await mouse(page, "mouseReleased", x, y, { button: "left", clickCount: 1 });
  };
  const shown = async (page) => { const t = await tagger(page); return !!t && /display:\s*block/.test(attr(t.host, "style") || ""); };
  const videoBox = (page) => page.eval(`(() => { const r = document.getElementById("v").getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })()`);
  const hover = async (page, vr) => { for (let i = 0; i < 5; i++) { await mouse(page, "mouseMoved", vr.x + 60 + i * 25, vr.y + vr.h / 2); await sleep(150); } };

  // ---- normal video ---------------------------------------------------------
  let page = await openPage("/tagpage/clip.mp4");
  ok("button hidden before the mouse comes", !(await shown(page)));
  let vr = await videoBox(page);
  await hover(page, vr);
  ok("button appears when the mouse is over the video", await waitFor(() => shown(page), 3000));
  let t = await tagger(page);
  const bar = await box(page, t.go);
  ok("button is a small chip in the top-right corner",
     bar && bar.w <= 100 && bar.h <= 30 && bar.x > vr.x + vr.w / 2 && bar.y >= vr.y && bar.y < vr.y + 40, JSON.stringify(bar));
  ok("chip says U-loader", textOf(t.go) === "U-loader", JSON.stringify(textOf(t.go)));

  await click(page, bar.x + bar.w / 2, bar.y + bar.h / 2);
  const labels = await waitFor(async () => {
    const tt = await tagger(page);
    if (attr(tt.menu, "hidden") !== null) return null;
    return tt.items().map((i) => textOf(i.children?.[0] ?? i));
  }, 4000);
  ok("menu lists the choices", labels && labels.includes("This video file") && labels.includes("Best quality from this page"),
     JSON.stringify(labels));
  const png = await page.send("Page.captureScreenshot", { format: "png", clip: { x: 0, y: 0, width: 760, height: 480, scale: 1 } });
  if (png.result?.data) writeFileSync(shot, Buffer.from(png.result.data, "base64"));

  t = await tagger(page);
  const first = await box(page, t.items()[0]);
  await click(page, first.x + 30, first.y + first.h / 2);
  const jobs = await waitFor(async () => { const j = await jobsSent(); return j && j.length ? j : null; }, 4000);
  const j = jobs && jobs[0];
  ok("click sends the video file to U-loader",
     j && j.url === `${base}/v/clip.mp4` && j.mode === "direct" && j.referer === `${base}/tagpage/clip.mp4`, JSON.stringify(j));
  const toast = await waitFor(async () => { const tt = await tagger(page); return attr(tt.toast, "hidden") === null ? textOf(tt.toast) : null; }, 3000);
  ok("confirmation shown", toast && toast.includes("Sent to U-loader"), JSON.stringify(toast));
  ok("clicks on the button do not reach the player", (await page.eval(`document.getElementById("v").paused`)) === true);

  // x hides it for this video
  t = await tagger(page);
  const xb = await box(page, t.x);
  await click(page, xb.x + xb.w / 2, xb.y + xb.h / 2);
  await sleep(300);
  await hover(page, vr);
  ok("x hides it, and it stays hidden for that video", !(await shown(page)));
  await page.close();

  // ---- a page cannot drive the button --------------------------------------
  page = await openPage("/tagpage/clip.mp4");
  vr = await videoBox(page);
  await hover(page, vr);                                     // the button exists once it was shown
  await waitFor(() => shown(page), 3000);
  await mouse(page, "mouseMoved", vr.x + vr.w + 150, vr.y + vr.h + 150);
  await waitFor(async () => !(await shown(page)), 9000);     // wait until it hid itself
  const sealed = await page.eval(`(() => { const h = document.querySelector("uloader-tagger"); return h ? h.shadowRoot === null : "no host"; })()`);
  ok("page script cannot reach the button (closed shadow root)", sealed === true, JSON.stringify(sealed));
  await page.eval(`(() => { const v = document.getElementById("v"); v.dispatchEvent(new Event("play"));
                     for (let i = 0; i < 5; i++) document.dispatchEvent(new MouseEvent("mousemove", { clientX: ${vr.x + 100} + i * 20, clientY: ${vr.y + 100}, bubbles: true }));
                     const h = document.querySelector("uloader-tagger"); if (h) h.click(); return true; })()`);
  await sleep(800);
  ok("fake play / mouse events from the page do not show it", !(await shown(page)));
  ok("nothing was sent by the page", (await jobsSent()).length === 1);

  // switched off in the settings: never shown
  await ext.eval(`chrome.storage.local.set({ tagger: false }).then(() => true)`);
  await sleep(300);
  await hover(page, vr);
  ok("setting off: no button", !(await shown(page)));
  await ext.eval(`chrome.storage.local.set({ tagger: true }).then(() => true)`);
  await sleep(300);
  await hover(page, vr);
  ok("setting back on: button again", await waitFor(() => shown(page), 3000));
  await page.close();

  // ---- a small video: icon only ---------------------------------------------
  page = await openPage("/tagpage/clip.mp4?w=280");
  vr = await videoBox(page);
  await hover(page, vr);
  ok("button appears on a small video", await waitFor(() => shown(page), 3000));
  t = await tagger(page);
  const small = await box(page, t.go);
  ok("small video: icon only", small && small.w <= 36, JSON.stringify(small));
  await page.close();

  // ---- a video at the right edge of the window: the label stays on one line --
  page = await openPage("/tagpage/clip.mp4?w=420&right=1");
  vr = await videoBox(page);
  await hover(page, vr);
  ok("button appears on a video at the window's edge", await waitFor(() => shown(page), 3000));
  t = await tagger(page);
  const edge = await box(page, t.go);
  const label = t.go && walk(t.go, (n) => n.localName === "span");
  const lb = label && await box(page, label);
  ok("at the window's edge: \"U-loader\" on one line, chip not squeezed",
     edge && lb && edge.h <= 26 && lb.h <= 20 && lb.w >= 40 && edge.x + edge.w <= vr.x + vr.w,
     JSON.stringify({ chip: edge, label: lb, video: vr }));
  const png2 = await page.send("Page.captureScreenshot", { format: "png", clip: { x: Math.max(0, vr.x + vr.w - 260), y: 0, width: 260, height: 140, scale: 1 } });
  if (png2.result?.data) writeFileSync(shot.replace(/\.png$/i, "") + "-edge.png", Buffer.from(png2.result.data, "base64"));
  await page.close();

  ok("every job went to the test sink (none to a real U-loader)", (await jobsSent()).length === 1);
  b.close(); ext.close();
} catch (e) {
  ok("unexpected error", false, String(e.stack || e));
} finally {
  clearTimeout(watchdog);
  proc.kill();
  await sleep(1200);
  try { rmSync(profile, { recursive: true, force: true }); } catch {}
  const failed = results.filter((r) => !r).length;
  console.log(`\n${results.length - failed} passed, ${failed} failed`);
  process.exit(failed ? 1 : 0);
}
