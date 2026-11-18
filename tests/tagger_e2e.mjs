// Real-browser test of the "Download this video" button (tagger.js).
//
//   node tests/tagger_e2e.mjs <browser.exe> <extension dir> <server base URL> <screenshot.png>
//
// The page is tests/server.py's /tagpage/<file>: a 640x360 video with a
// transparent overlay on top (like YouTube's controls). Sending to
// U-loader is intercepted inside the extension, so nothing is downloaded.
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

const watchdog = setTimeout(() => { console.log("FAIL  watchdog: test took over 120 s"); proc.kill(); process.exit(1); }, 120000);
try {
  const version = await waitFor(() => json("/json/version"), 20000);
  const sw = await waitFor(async () => (await json("/json/list")).find(
    (t) => t.type === "service_worker" && t.url.startsWith(`chrome-extension://${EXT_ID}/`)), 20000);
  ok("extension loaded", sw);
  const ext = await cdp(sw.webSocketDebuggerUrl);
  // intercept the hand-over to U-loader: record jobs instead of sending them
  await ext.eval(`self.__jobs = []; sendToUloader = async (job) => { self.__jobs.push(job); return { ok: true }; }; true`);

  const b = await cdp(version.webSocketDebuggerUrl);
  const tgt = (await b.send("Target.createTarget", { url: `${base}/tagpage/clip.mp4` })).result.targetId;
  const pageInfo = await waitFor(async () => (await json("/json/list")).find((t) => t.id === tgt), 10000);
  const page = await cdp(pageInfo.webSocketDebuggerUrl);
  await page.send("Page.bringToFront");
  await waitFor(() => page.eval(`!!document.querySelector("uloader-tagger") || document.readyState === "complete"`), 10000);
  await sleep(800);

  const host = `document.querySelector("uloader-tagger")`;
  const shown = () => page.eval(`(() => { const h = ${host}; return !!h && h.style.display === "block"; })()`);
  const mouse = (type, x, y, extra = {}) => page.send("Input.dispatchMouseEvent", { type, x, y, button: "none", ...extra });

  ok("button hidden before the mouse comes", !(await shown()));
  const vr = await page.eval(`(() => { const r = document.getElementById("v").getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })()`);
  // move over the video - through the transparent overlay, like on YouTube
  for (let i = 0; i < 5; i++) { await mouse("mouseMoved", vr.x + 100 + i * 40, vr.y + 150); await sleep(150); }
  ok("button appears when the mouse is over the video", await waitFor(shown, 3000));
  const br = await page.eval(`(() => { const r = ${host}.shadowRoot.querySelector(".go").getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })()`);
  ok("button sits inside the video's top-right corner",
     br.x > vr.x + vr.w / 2 && br.x + br.w <= vr.x + vr.w && br.y >= vr.y && br.y < vr.y + 60, JSON.stringify(br));

  // click the button: a menu with the choices
  await mouse("mouseMoved", br.x + 20, br.y + 10);
  await mouse("mousePressed", br.x + 20, br.y + 10, { button: "left", clickCount: 1 });
  await mouse("mouseReleased", br.x + 20, br.y + 10, { button: "left", clickCount: 1 });
  const items = await waitFor(() => page.eval(`(() => { const m = ${host}.shadowRoot.querySelector(".menu");
      return m.hidden ? null : [...m.querySelectorAll(".item .label")].map((e) => e.textContent); })()`), 4000);
  ok("menu lists the choices", items && items.includes("This video file") && items.includes("Best quality from this page"),
     JSON.stringify(items));

  const png = await page.send("Page.captureScreenshot", { format: "png", clip: { x: 0, y: 0, width: 760, height: 520, scale: 1 } });
  if (png.result?.data) writeFileSync(shot, Buffer.from(png.result.data, "base64"));

  // choose "This video file"
  const ir = await page.eval(`(() => { const r = ${host}.shadowRoot.querySelector(".menu .item").getBoundingClientRect(); return { x: r.left, y: r.top }; })()`);
  await mouse("mousePressed", ir.x + 30, ir.y + 12, { button: "left", clickCount: 1 });
  await mouse("mouseReleased", ir.x + 30, ir.y + 12, { button: "left", clickCount: 1 });
  const jobs = await waitFor(async () => { const j = await ext.eval("self.__jobs"); return j && j.length ? j : null; }, 4000);
  const j = jobs && jobs[0];
  ok("click sends the video file to U-loader",
     j && j.url === `${base}/v/clip.mp4` && j.mode === "direct" && j.referer === `${base}/tagpage/clip.mp4`, JSON.stringify(j));
  const toast = await waitFor(() => page.eval(`(() => { const t = ${host}.shadowRoot.querySelector(".toast"); return t.hidden ? null : t.textContent; })()`), 3000);
  ok("confirmation shown", toast && toast.includes("Sent to U-loader"), JSON.stringify(toast));

  // the video player must not see our clicks
  const paused = await page.eval(`document.getElementById("v").paused`);
  ok("clicks on the button do not reach the player", paused === true);

  // x hides it for this video
  const xr = await page.eval(`(() => { const r = ${host}.shadowRoot.querySelector(".x").getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; })()`);
  await mouse("mousePressed", xr.x + xr.w / 2, xr.y + xr.h / 2, { button: "left", clickCount: 1 });
  await mouse("mouseReleased", xr.x + xr.w / 2, xr.y + xr.h / 2, { button: "left", clickCount: 1 });
  await sleep(300);
  for (let i = 0; i < 4; i++) { await mouse("mouseMoved", vr.x + 200 + i * 30, vr.y + 200); await sleep(150); }
  ok("x hides it, and it stays hidden for that video", !(await shown()));

  // switched off in the settings: never shown
  await page.eval(`location.reload(); true`);
  await sleep(1500);
  await ext.eval(`chrome.storage.local.set({ tagger: false }).then(() => true)`);
  await sleep(300);
  for (let i = 0; i < 4; i++) { await mouse("mouseMoved", vr.x + 120 + i * 30, vr.y + 120); await sleep(150); }
  ok("setting off: no button", !(await shown()));
  await ext.eval(`chrome.storage.local.set({ tagger: true }).then(() => true)`);
  await sleep(300);
  for (let i = 0; i < 4; i++) { await mouse("mouseMoved", vr.x + 150 + i * 30, vr.y + 160); await sleep(150); }
  ok("setting back on: button again", await waitFor(shown, 3000));

  page.close(); b.close(); ext.close();
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
