// Renders assets/icon0.png (512x512): the memory card model on the page's background.
//   node tools/make-icon.cjs [out.png] [query]      e.g.  node tools/make-icon.cjs /tmp/a.png "bold=1&text=PS5"
// Needs the dev dependency (npm install) and a Chromium that Playwright can start.
const { chromium } = require('@playwright/test');
const http = require('http'), fs = require('fs'), path = require('path');
const ROOT = path.join(__dirname, '..');
const out = process.argv[2] || path.join(ROOT, 'assets/icon0.png');
const query = process.argv[3] || 'bold=1&text=PS5%20saves&style=italic%20500&family=Z003';
const files = { '/': ['tools/icon.html', 'text/html'], '/card3d.js': ['src/card3d.js', 'text/javascript'], '/three.module.min.js': ['vendor/three.module.min.js', 'text/javascript'] };
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
    const page = await browser.newPage({ viewport: { width: 512, height: 512 }, deviceScaleFactor: 1 });
    page.on('pageerror', e => { throw e; });
    await page.goto(`http://127.0.0.1:${server.address().port}/?${query}`);
    await page.waitForFunction(() => window.mcIcons3d === true && window.mcIcons.get('icon').drawn3d);
    await page.waitForTimeout(200);
    await page.locator('#icon').screenshot({ path: out });
    console.log('wrote', out);
  } finally { await browser.close(); server.close(); }
})().catch(e => { console.error(e); process.exit(1); });
