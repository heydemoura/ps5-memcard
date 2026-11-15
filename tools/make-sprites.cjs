// Renders assets/cards.webp + assets/cards.json: the turning memory card, frame by frame, for
// browsers without WebGL (the PS5's own browser has none). The page plays these frames on a
// 2D canvas and writes the card's name on the label, using the label corners kept per frame.
//   node tools/make-sprites.cjs          (make sprites)
// Needs the dev dependency (npm install) and a Chromium that Playwright can start.
const { chromium } = require('@playwright/test');
const http = require('http'), fs = require('fs'), path = require('path');
const ROOT = path.join(__dirname, '..');
const N = 96, COLS = 16, W = 104, H = 136, STILL = -0.55, QUALITY = 0.86;
const files = { '/': ['tools/sprites.html', 'text/html'], '/card3d.js': ['src/card3d.js', 'text/javascript'], '/three.module.min.js': ['vendor/three.module.min.js', 'text/javascript'] };
const system = process.env.CHROMIUM_PATH || ['/usr/bin/chromium', '/usr/bin/chromium-browser'].find(p => fs.existsSync(p));

(async () => {
  const server = http.createServer((req, res) => {
    const f = files[req.url.split('?')[0]];
    if (!f) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'content-type': f[1] }); res.end(fs.readFileSync(path.join(ROOT, f[0])));
  }).listen(0, '127.0.0.1');
  await new Promise(r => server.on('listening', r));
  const browser = await chromium.launch(system ? { executablePath: system } : {});
  try {
    const page = await browser.newPage({ deviceScaleFactor: 1 });
    page.on('pageerror', e => { throw e; });
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.waitForFunction(() => window.mcIcons3d === true && window.mcIcons.get('card').drawn3d);
    const out = await page.evaluate(async ({ N, COLS, W, H, STILL, QUALITY }) => {
      const frames = 2 * N + 1, rows = Math.ceil(frames / COLS);
      const sheet = document.createElement('canvas'); sheet.width = COLS * W; sheet.height = rows * H;
      const sc = sheet.getContext('2d');
      const next = () => new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r)));
      const grab = async (key, angle, at) => {
        const e = window.mcIcons.get(key); e.angle = angle; e.dirty = true; e.drawn3d = false;
        while (!e.drawn3d) await next();
        sc.drawImage(e.canvas, (at % COLS) * W, Math.floor(at / COLS) * H);
        return e.label;
      };
      const card = [];
      for (let i = 0; i < N; i++) card.push(await grab('card', i * 2 * Math.PI / N, i));
      for (let i = 0; i < N; i++) await grab('all', i * 2 * Math.PI / N, N + i);
      const still = await grab('card', STILL, 2 * N);
      return { webp: sheet.toDataURL('image/webp', QUALITY), meta: { w: W, h: H, n: N, cols: COLS, speed: 0.0011, still: 2 * N, card, stillLabel: still } };
    }, { N, COLS, W, H, STILL, QUALITY });
    fs.writeFileSync(path.join(ROOT, 'assets/cards.webp'), Buffer.from(out.webp.split(',')[1], 'base64'));
    fs.writeFileSync(path.join(ROOT, 'assets/cards.json'), JSON.stringify(out.meta) + '\n');
    console.log('wrote assets/cards.webp', fs.statSync(path.join(ROOT, 'assets/cards.webp')).size, 'bytes, assets/cards.json');
  } finally { await browser.close(); server.close(); }
})().catch(e => { console.error(e); process.exit(1); });
