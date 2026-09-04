// Headless-Chrome screenshot of the browser editor through CDP, with the page
// console captured. Nothing is shown on screen (--headless=new).
//   node shoot.js <url> <out.png> <console.log> [sceneTime] [playSeconds]
const { spawn } = require('child_process');
const fs = require('fs');

const [url, outPng, logPath, tArg, playArg] = process.argv.slice(2);
const sceneTime = tArg ? Number(tArg) : 600;
const playSeconds = playArg ? Number(playArg) : 0;
const port = 9600 + Math.floor(Math.random() * 300);

async function main() {
  const tmpDir = '/private/tmp/claude-501/-Users-gil-ad-work-revival-fog/7ec4ee82-204d-4fb8-a259-a95020066560/scratchpad/gpuweb/chrome_' + Date.now();
  fs.mkdirSync(tmpDir, { recursive: true });
  const chrome = spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome', [
    '--headless=new', '--remote-debugging-port=' + port, '--user-data-dir=' + tmpDir,
    '--window-size=1400,900', '--no-first-run', '--disable-extensions',
    '--use-angle=metal', '--enable-unsafe-webgpu', url]);
  let pageWs = null;
  for (let i = 0; i < 60 && !pageWs; ++i) {
    await new Promise(r => setTimeout(r, 250));
    try {
      const list = await (await fetch('http://localhost:' + port + '/json')).json();
      const p = list.find(x => x.type === 'page');
      if (p && p.webSocketDebuggerUrl) pageWs = p.webSocketDebuggerUrl;
    } catch (e) {}
  }
  if (!pageWs) { chrome.kill(); throw new Error('no CDP page'); }
  const ws = new WebSocket(pageWs);
  let id = 1; const cbs = new Map(); const log = [];
  const send = (method, params = {}) => new Promise(res => { const i = id++; cbs.set(i, res); ws.send(JSON.stringify({ id: i, method, params })); });
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && cbs.has(m.id)) { cbs.get(m.id)(m.result); cbs.delete(m.id); return; }
    if (m.method === 'Runtime.consoleAPICalled') log.push('[console.' + m.params.type + '] ' + m.params.args.map(a => a.value !== undefined ? String(a.value) : (a.description || a.type)).join(' '));
    if (m.method === 'Runtime.exceptionThrown') log.push('[exception] ' + JSON.stringify(m.params.exceptionDetails).slice(0, 600));
    if (m.method === 'Log.entryAdded') log.push('[log.' + m.params.entry.level + '] ' + m.params.entry.text);
  };
  await new Promise(r => ws.onopen = r);
  await send('Runtime.enable'); await send('Page.enable'); await send('Log.enable');
  // wait for the editor scene
  let ready = false;
  for (let i = 0; i < 120 && !ready; ++i) {
    await new Promise(r => setTimeout(r, 500));
    const r = await send('Runtime.evaluate', { expression: 'Boolean(window.editorSurfaces && window.editorSurfaces.length > 0)' });
    ready = !!(r && r.result && r.result.value);
  }
  log.push('[shoot] scene ready=' + ready);
  const tr = await send('Runtime.evaluate', { expression: 'Module.editorTimeSet ? Module.editorTimeSet(' + sceneTime + ') : "no editorTimeSet"' });
  log.push('[shoot] editorTimeSet -> ' + (tr && tr.result ? tr.result.value : '?'));
  await new Promise(r => setTimeout(r, 1500));
  if (playSeconds > 0) {
    const pr = await send('Runtime.evaluate', { expression: 'Module.editorPlayScene ? Module.editorPlayScene(true) : "no editorPlayScene"' });
    log.push('[shoot] editorPlayScene -> ' + (pr && pr.result ? pr.result.value : '?'));
    await new Promise(r => setTimeout(r, playSeconds * 1000));
  }
  const shot = await send('Page.captureScreenshot', { format: 'png' });
  if (shot && shot.data) fs.writeFileSync(outPng, Buffer.from(shot.data, 'base64'));
  const info = await send('Runtime.evaluate', { expression: 'JSON.stringify({gpu: Module.editorGetGpuMode ? Module.editorGetGpuMode() : null, canvas: [Module.canvas.width, Module.canvas.height], rect: (function(){var r=Module.canvas.getBoundingClientRect();return [r.left,r.top,r.width,r.height];})()})' });
  log.push('[shoot] ' + (info && info.result ? info.result.value : '?'));
  fs.writeFileSync(logPath, log.join('\n') + '\n');
  ws.close(); chrome.kill();
  console.log('saved', outPng, 'log lines', log.length);
}
main().catch(e => { console.error(e); process.exit(1); });
