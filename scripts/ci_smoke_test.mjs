// CI smoke test: build-wasm/DEMO/DEMO.html under headless Chromium,
// click to start, render for ~10s, screenshot, and verify the canvas
// produced *something* (not just solid black/white).
//
// Catches regressions like:
//   - 404 on audio-worklet.js (which silently breaks audio)
//   - missing _site/* file in the gh-pages workflow
//   - rendering pipeline broken so canvas stays black
//
// Usage:  node scripts/ci_smoke_test.mjs
// Exit 0 = pass; exit 1 = fail with a stderr explanation.

import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { chromium } from "playwright";

const __dirname = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(__dirname, "..");
const wasmDir = resolve(repoRoot, "build-wasm/DEMO");

const port = 8765;
const server = spawn("python3", ["-m", "http.server", String(port)],
	{ cwd: wasmDir, stdio: ["ignore", "ignore", "inherit"] });
process.on("exit", () => server.kill());
await new Promise((r) => setTimeout(r, 500));

const browser = await chromium.launch({ headless: true });
const ctx = await browser.newContext({
	deviceScaleFactor: 2,
	viewport: { width: 1280, height: 720 },
});
const page = await ctx.newPage();

const consoleErrors = [];
const pageErrors = [];
page.on("console", (m) => {
	if (m.type() === "error") consoleErrors.push(m.text());
});
page.on("pageerror", (e) => pageErrors.push(e.message));

let rc = 0;
try {
	await page.goto(`http://localhost:${port}/DEMO.html`, {
		waitUntil: "load",
		timeout: 20000,
	});
	// Service worker reload + boot.
	await new Promise((r) => setTimeout(r, 2000));
	// Click + key to satisfy user-gesture gate (audio + scene start).
	await page.evaluate(() => document.getElementById("canvas")?.focus());
	await page.mouse.move(640, 360);
	await page.mouse.down();
	await page.mouse.up();
	await page.keyboard.press("Space");
	// Let Glat render for ~10s. By 10s the user gesture has been
	// observed, scene init has finished, and we're well into Glat.
	await new Promise((r) => setTimeout(r, 10000));

	// Sample the canvas. If everything's working, Glat is animated
	// blue/red blobs — wide range of colors. If broken (404 worklet,
	// black canvas, etc.), we get uniform black.
	const stats = await page.evaluate(() => {
		const c = document.getElementById("canvas");
		if (!c) return { error: "no canvas" };
		const ctx = c.getContext("2d");
		if (!ctx) return { error: "no 2d context (canvas in webgl mode)" };
		// canvas might be webgl — fall back to grabbing image data via
		// drawImage path
		return { error: "canvas is webgl, can't read directly" };
	});
	// Better path: screenshot the canvas element and inspect the image.
	const buf = await page.locator("#canvas").screenshot({ omitBackground: false });
	// Decode minimum we need: count distinct pixel values.
	// PNG header check + cheap variance — for proper variance we'd parse
	// PNG, but for "is it solid black" a simple byte-distribution check
	// on the file is enough: a solid-black PNG compresses to a tiny
	// file, while a textured frame is much larger.
	const sizeKB = (buf.length / 1024) | 0;
	console.log(`[smoke] canvas screenshot ${buf.length} bytes (${sizeKB} KB)`);
	if (buf.length < 8 * 1024) {
		console.error(
			`[smoke] FAIL: canvas screenshot only ${sizeKB} KB; ` +
			`a rendered Glat frame should be 100+ KB. Likely a black canvas.`
		);
		rc = 1;
	}

	// Also fail on uncaught page errors.
	if (pageErrors.length) {
		console.error(`[smoke] FAIL: page errors:`);
		for (const e of pageErrors) console.error("  - " + e);
		rc = 1;
	}
} catch (e) {
	console.error(`[smoke] FAIL: navigation/test error: ${e.message}`);
	rc = 1;
}

if (consoleErrors.length) {
	console.log(`[smoke] (info) ${consoleErrors.length} console.error msgs during run`);
}

await browser.close();
server.kill();
process.exit(rc);
