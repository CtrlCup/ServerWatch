// Rendert das ServerWatch-Dashboard in jsdom und gibt das Ergebnis als JSON aus.
// Eingabe (JSON-Datei als argv[2]):
//   { html, url, statusResponse, wsMessages: [...], click: ["css-selector", ...] }
// WebSocket und fetch werden gestubbt: fetch('/api/status') liefert statusResponse,
// jede WebSocket-Instanz bekommt nacheinander die wsMessages.
import fs from "node:fs";
import { JSDOM, VirtualConsole } from "jsdom";

const input = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const errors = [];
const fetchCalls = [];
const vc = new VirtualConsole();
vc.on("jsdomError", (e) => errors.push(String(e && e.message ? e.message : e)));

const dom = new JSDOM(input.html, {
  url: input.url || "http://127.0.0.1/",
  runScripts: "dangerously",
  pretendToBeVisual: true,
  virtualConsole: vc,
  beforeParse(window) {
    window.__wsInstances = [];
    window.WebSocket = class {
      constructor(url) {
        this.url = url;
        window.__wsInstances.push(this);
        setTimeout(() => this.onopen && this.onopen(), 0);
      }
      send() {}
      close() {}
    };
    window.fetch = (url, opts = {}) => {
      fetchCalls.push({ url: String(url), method: opts.method || "GET", body: opts.body || null });
      const body = String(url).includes("/api/status") ? input.statusResponse : { success: true };
      return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(body) });
    };
  },
});
const w = dom.window;
await sleep(80);
for (const m of input.wsMessages || []) {
  for (const ws of w.__wsInstances) ws.onmessage && ws.onmessage({ data: typeof m === "string" ? m : JSON.stringify(m) });
  await sleep(30);
}
for (const sel of input.click || []) {
  w.document.querySelectorAll(sel).forEach((b) => b.click());
  await sleep(30);
}
await sleep(50);

const grid = w.document.getElementById("serverGrid");
const cards = [...grid.querySelectorAll(".card")].map((c) => ({
  title: c.querySelector(".card-title")?.textContent.trim(),
  badge: c.querySelector(".badge")?.textContent.trim(),
  info: [...c.querySelectorAll(".info-row")].map((r) => [...r.children].map((x) => x.textContent.trim()).join(": ")),
  buttons: [...c.querySelectorAll("button")].map((b) => ({
    text: b.textContent.trim(),
    disabled: b.disabled,
    onclick: b.getAttribute("onclick"),
  })),
}));
const result = {
  cards,
  injectedElements: grid.querySelectorAll("img, script, iframe, svg[onload], [onerror], [onload]").length,
  xss: w.__xss === 1,
  fetchCalls,
  errors,
  stats: {
    total: w.document.getElementById("totalServers")?.textContent,
    online: w.document.getElementById("onlineServers")?.textContent,
    esps: w.document.getElementById("connectedESPs")?.textContent,
  },
};
w.close();
process.stdout.write(JSON.stringify(result));
process.exit(0);
