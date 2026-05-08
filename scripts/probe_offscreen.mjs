// Headless Firefox boot of build-wasm/DEMO/DEMO.html, capturing console.
// Used to debug OffscreenCanvas transferability without round-tripping
// through the human.
//
// Usage:
//   node scripts/probe_offscreen.mjs                  -> default (Firefox, 8s)
//   node scripts/probe_offscreen.mjs chromium 15      -> Chromium, 15s
//
// Spawns a tiny Python http.server that sends Cache-Control: no-store
// (matches `make serve`), navigates to DEMO.html, dumps every console
// message + page error to stdout.

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { firefox, chromium } from "playwright";

const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(__dirname, "..");
const wasmDir = resolve(repoRoot, "build-wasm/DEMO");

const browserName = process.argv[2] || "firefox";
const seconds = parseInt(process.argv[3] || "8", 10);

const port = 8765;
const serverScript = resolve(repoRoot, "scripts/serve_nocache.py");
const server = spawn("python3", [serverScript, String(port)], {
	cwd: wasmDir,
	stdio: ["ignore", "ignore", "inherit"],
});
process.on("exit", () => server.kill());

await new Promise((r) => setTimeout(r, 400)); // server warmup

const launcher = browserName === "chromium" ? chromium : firefox;
const headless = process.argv[4] !== "headed";
const browser = await launcher.launch({ headless });
const ctx = await browser.newContext();
const page = await ctx.newPage();

page.on("console", (m) => console.log(`[${m.type()}] ${m.text()}`));
page.on("pageerror", (e) => console.log(`[pageerror] ${e.message}`));

const url = `http://localhost:${port}/DEMO.html`;
console.log(`[probe] ${browserName} -> ${url} for ${seconds}s`);
try {
	await page.goto(url, { waitUntil: "load", timeout: 15000 });
} catch (e) {
	console.log(`[probe] navigation: ${e.message}`);
}

// Wait for the COOP/COEP service worker reload to settle, then click
// to trigger the user-gesture gate (the demo blocks on a click before
// audio + scene start).
await new Promise((r) => setTimeout(r, 1500));
try { await page.click("#canvas", { force: true, timeout: 1500 }); } catch (_) {}
try { await page.keyboard.press("Space"); } catch (_) {}
await new Promise((r) => setTimeout(r, seconds * 1000));
// Visual snapshot — useful for verifying the canvas actually renders
// (FLIP timings alone don't catch an OffscreenCanvas-placeholder bridge
// failure that produces a black canvas with sub-millisecond GL ops).
try {
	await page.screenshot({ path: "/tmp/probe_screenshot.png", fullPage: false });
	console.log("[probe] screenshot -> /tmp/probe_screenshot.png");
} catch (e) {
	console.log(`[probe] screenshot: ${e.message}`);
}
await browser.close();
server.kill();
process.exit(0);
