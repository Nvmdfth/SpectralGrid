// Real-time headless-Chromium driver over the DevTools protocol (the plain --screenshot flag runs in virtual time, which
// cannot finish real-time work such as MediaRecorder or WebSocket traffic).
// usage: node --experimental-websocket browser_check.mjs <url> <steps.json> [width] [height] [--fake-mic]
// steps: [{"wait":ms}, {"eval":"js","print":true}, {"click":"#selector"}, {"shot":"/path.jpg"}, {"waitfor":"js returning truthy","timeout":ms}]
import { spawn } from 'node:child_process';
import { readFileSync, writeFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [url, stepsFile, w = '1100', h = '1500', ...flags] = process.argv.slice(2);
const steps = JSON.parse(readFileSync(stepsFile, 'utf8'));
const dir = mkdtempSync(join(tmpdir(), 'chrome-'));
const port = 9300 + Math.floor(Math.random() * 500);
const args = ['--headless=new', '--no-sandbox', '--disable-gpu', '--hide-scrollbars', `--remote-debugging-port=${port}`, `--user-data-dir=${dir}`, `--window-size=${w},${h}`, 'about:blank'];
if (flags.includes('--fake-mic')) args.splice(1, 0, '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream');
const chrome = spawn('chromium', args, { stdio: 'ignore' });
const sleep = ms => new Promise(r => setTimeout(r, ms));
async function connect() {
  for (let i = 0; i < 60; i++) {
    try { const r = await fetch(`http://127.0.0.1:${port}/json`); const t = (await r.json()).find(x => x.type === 'page'); if (t) return t.webSocketDebuggerUrl; } catch {}
    await sleep(250);
  }
  throw new Error('chromium did not start');
}
let id = 0; const pending = new Map();
const ws = new WebSocket(await connect());
await new Promise(r => ws.addEventListener('open', r));
ws.addEventListener('message', e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } });
const send = (method, params = {}) => new Promise(r => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
const evalJs = async expr => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); if (r.result.exceptionDetails) throw new Error(JSON.stringify(r.result.exceptionDetails.exception?.description || r.result.exceptionDetails.text)); return r.result.result.value; };
try {
  await send('Page.enable'); await send('Runtime.enable');
  await send('Emulation.setDeviceMetricsOverride', { width: +w, height: +h, deviceScaleFactor: 1, mobile: +w < 600 });
  await send('Page.navigate', { url });
  await sleep(1500);
  for (const s of steps) {
    if (s.wait) await sleep(s.wait);
    if (s.click) await evalJs(`document.querySelector(${JSON.stringify(s.click)}).click()`);
    if (s.eval) { const v = await evalJs(s.eval); if (s.print) console.log(JSON.stringify(v)); }
    if (s.waitfor) { const t0 = Date.now(); let ok = false; while (Date.now() - t0 < (s.timeout || 15000)) { if (await evalJs(s.waitfor)) { ok = true; break; } await sleep(250); } console.log('waitfor', ok ? 'ok' : 'TIMEOUT', JSON.stringify(s.waitfor).slice(0, 60)); }
    if (s.shot) { const r = await send('Page.captureScreenshot', { format: s.shot.endsWith('.png') ? 'png' : 'jpeg', quality: 90 }); writeFileSync(s.shot, Buffer.from(r.result.data, 'base64')); console.log('shot', s.shot); }
  }
} finally { try { await send('Browser.close'); } catch {} chrome.kill(); await sleep(800); try { rmSync(dir, { recursive: true, force: true, maxRetries: 5, retryDelay: 300 }); } catch {} process.exit(0); }
