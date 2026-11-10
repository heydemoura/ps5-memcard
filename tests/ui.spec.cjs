// End-to-end: the real page + the real memcard core, running in the PC simulator
// (tests/sim.c) against a throwaway fake console. Build first: make build/sim build/ui.html
const { test, expect } = require('@playwright/test');
const { spawn } = require('child_process');
const fs = require('fs');
const os = require('os');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const SHOTS = process.env.MC_SHOTS;
let sim, root, base, port = 18200;

async function startSim(args = []) {
  root = fs.mkdtempSync(path.join(os.tmpdir(), 'memcard-e2e-'));
  fs.rmdirSync(root);
  port++;
  base = `http://127.0.0.1:${port}`;
  sim = spawn(path.join(ROOT, 'build/sim'), ['--port', String(port), '--ui', path.join(ROOT, 'build/ui.html'), '--root', root, ...args],
    { stdio: ['ignore', 'pipe', 'pipe'] });
  await new Promise((resolve, reject) => {
    sim.stdout.on('data', d => String(d).includes('simulator:') && resolve());
    sim.on('exit', code => reject(new Error('sim exited ' + code)));
  });
}
const api = async (p, opt) => (await fetch(base + p, opt)).json();
const post = (p, body) => api(p, { method: 'POST', ...(body ? { body: JSON.stringify(body) } : {}) });
const fast = () => post('/api/mc/settings', { poll_seconds: 2, settle_seconds: 1 });
const home = (...p) => path.join(root, 'home', ...p);
const card = (...p) => path.join(root, 'usb0', 'memcard', ...p);
const cardOn = (n, ...p) => path.join(root, 'usb' + n, 'memcard', ...p);
const cardsInUse = async n => expect.poll(async () => (await api('/api/mc/status')).card.count, { timeout: 30000 }).toBe(n);
const synced = async n => expect.poll(async () => (await api('/api/mc/status')).watcher.synced, { timeout: 30000 }).toBe(n);
const shot = async (page, name) => { if (SHOTS) await page.screenshot({ path: path.join(SHOTS, name), fullPage: true }); };

test.afterEach(async () => {
  if (sim) sim.kill();
  if (root && fs.existsSync(root)) fs.rmSync(root, { recursive: true, force: true });
});

test('the card tab lists what was mirrored, grouped by game', async ({ page }) => {
  await startSim();
  await synced(7);
  await page.goto(base);
  await expect(page.locator('#cardpill')).toContainText('usb0');
  await expect(page.locator('#sCard')).toHaveText('7');
  await expect(page.locator('#sSync')).toHaveText('7');
  await expect(page.locator('#sWait')).toHaveText('0');
  await expect(page.locator('.game')).toHaveCount(5);
  const got = page.locator('.game', { hasText: 'Ghost of Tsushima' });
  await expect(got.locator('.srow')).toHaveCount(2); // the sce_bu system copy is not mirrored
  await expect(got).toContainText('Autosave');
  await expect(got).toContainText('from heyde');
  await expect(page.locator('.game', { hasText: 'Stellar Blade' })).toContainText('from player-two');
  await expect(page.locator('.game', { hasText: 'God of War' })).toContainText('PS4');
  await expect(page.locator('#watchText')).toContainText('Watching 7 saves');
  await expect(page.locator('#term')).toContainText('memory card ready on usb0');
  // bytes on the card are the bytes on the console
  expect(fs.readFileSync(card('10000000/PPSA02530/sdimg_RETURNAL0000')).equals(
    fs.readFileSync(home('10000000/savedata_prospero/PPSA02530/sdimg_RETURNAL0000')))).toBe(true);
  expect(fs.existsSync(card('10000000/CUSA07408/GOWSAVE0000.bin'))).toBe(true);
  await page.fill('#q', 'stellar');
  await expect(page.locator('.game')).toHaveCount(1);
  await page.fill('#q', '');
  await shot(page, 'card-desktop.png');
});

test('a save that changes is picked up and copied without being asked', async ({ page }) => {
  await startSim();
  await fast();
  await synced(7);
  await page.goto(base);
  await page.click('button[data-tab=console]');
  const row = page.locator('.srow', { hasText: 'Slot 1' }).filter({ hasText: 'heyde' }).first();
  await expect(page.locator('.chip.ok')).toHaveCount(7);
  const save = home('10000000/savedata_prospero/PPSA02530/sdimg_RETURNAL0000');
  fs.writeFileSync(save, Buffer.concat([Buffer.from([2]), Buffer.from('a brand new run of the game')]));
  await expect.poll(() => fs.readFileSync(card('10000000/PPSA02530/sdimg_RETURNAL0000')).toString('latin1'), { timeout: 30000 })
    .toContain('a brand new run');
  expect(fs.existsSync(card('10000000/PPSA02530/sdimg_RETURNAL0000.prev'))).toBe(true);
  await expect(page.locator('#term')).toContainText('heyde: Returnal / sdimg_RETURNAL0000');
  await expect(row).toBeVisible();
  await shot(page, 'console-desktop.png');
});

test('manual backup works with automatic backup switched off', async ({ page }) => {
  await startSim();
  await fast();
  await synced(7);
  await page.goto(base);
  await page.click('#set summary');
  await page.uncheck('#cEnabled');
  await page.click('#saveSet');
  await expect(page.locator('#watchText')).toContainText('Automatic backup is off');
  expect(JSON.parse(fs.readFileSync(path.join(root, 'data/config.json'), 'utf8')).enabled).toBe(false);

  const save = home('1b2c3d4e/savedata_prospero/PPSA08338/sdimg_STELLAR0000');
  fs.writeFileSync(save, Buffer.concat([Buffer.from([2]), Buffer.from('manual only')]));
  await page.click('button[data-tab=console]');
  const row = page.locator('.game', { hasText: 'Stellar Blade' }).locator('.srow');
  await expect(row.locator('.chip')).toHaveText('WAITING', { timeout: 30000 });
  await page.waitForTimeout(5000); // several watcher passes: still not copied
  expect(fs.readFileSync(card('1b2c3d4e/PPSA08338/sdimg_STELLAR0000')).toString('latin1')).not.toContain('manual only');
  await row.getByRole('button', { name: 'BACK UP' }).click();
  await expect.poll(() => fs.readFileSync(card('1b2c3d4e/PPSA08338/sdimg_STELLAR0000')).toString('latin1'), { timeout: 30000 })
    .toContain('manual only');
  await expect(row.locator('.chip')).toHaveText('ON CARD', { timeout: 30000 });
  await expect(page.locator('#term')).toContainText('manual: player-two: Stellar Blade / sdimg_STELLAR0000');
});

test('a card save is imported into the profile that was picked', async ({ page }) => {
  await startSim();
  await synced(7);
  await page.goto(base);
  const target = home('10000000/savedata_prospero/PPSA08338/sdimg_STELLAR0000');
  expect(fs.existsSync(target)).toBe(false);
  await page.locator('.game', { hasText: 'Stellar Blade' }).getByRole('button', { name: 'IMPORT' }).click();
  await expect(page.locator('#veil')).toBeVisible();
  await expect(page.locator('#dTitle')).toContainText('Stellar Blade');
  await expect(page.locator('#dGo')).toBeDisabled(); // two profiles: one has to be chosen
  await expect(page.locator('#dUsers button')).toHaveCount(2);
  await expect(page.locator('#dPrevBox')).toBeHidden(); // nothing older to go back to
  await shot(page, 'import-dialog.png');
  await page.locator('#dUsers button', { hasText: 'heyde' }).click();
  await expect(page.locator('#dWarn')).toBeHidden();
  await page.click('#dGo');
  await expect(page.locator('#veil')).toBeHidden({ timeout: 15000 });
  expect(fs.readFileSync(target).equals(fs.readFileSync(card('1b2c3d4e/PPSA08338/sdimg_STELLAR0000')))).toBe(true);
  await expect(page.locator('#term')).toContainText('1b2c3d4e/PPSA08338/sdimg_STELLAR0000 imported to heyde');
  expect(fs.existsSync(path.join(root, 'data/import'))).toBe(false); // staging is cleaned up
  // the imported save now belongs to heyde on the console, and gets mirrored for heyde too
  await page.click('button[data-tab=console]');
  await expect(page.locator('.game', { hasText: 'Stellar Blade' }).locator('.srow')).toHaveCount(2);
});

test('importing over an existing save asks first, and the previous copy can be restored', async ({ page }) => {
  await startSim();
  await fast();
  await synced(7);
  const save = home('10000000/savedata_prospero/PPSA01325/sdimg_ASTRO0000');
  const original = fs.readFileSync(save);
  fs.writeFileSync(save, Buffer.concat([Buffer.from([2]), Buffer.from('progress I regret')]));
  await expect.poll(() => fs.existsSync(card('10000000/PPSA01325/sdimg_ASTRO0000.prev')), { timeout: 30000 }).toBe(true);
  await page.goto(base);
  await page.locator('.game', { hasText: "Astro's Playroom" }).locator('.srow', { hasText: 'from heyde' })
    .getByRole('button', { name: 'IMPORT' }).click();
  await page.locator('#dUsers button', { hasText: 'heyde' }).click();
  await expect(page.locator('#dWarn')).toContainText('already has this save');
  await expect(page.locator('#dGo')).toHaveText('REPLACE & IMPORT ▸');
  await page.check('#dPrev');
  await page.click('#dGo');
  await expect(page.locator('#veil')).toBeHidden({ timeout: 15000 });
  expect(fs.readFileSync(save).equals(original)).toBe(true);
});

test('importing the save of a running game is refused', async ({ page }) => {
  await startSim();
  await synced(7);
  await post('/sim/running?title=PPSA08338');
  await page.goto(base);
  await expect(page.locator('#watchText')).toContainText('playing Stellar Blade');
  await page.locator('.game', { hasText: 'Stellar Blade' }).getByRole('button', { name: 'IMPORT' }).click();
  await page.locator('#dUsers button', { hasText: 'heyde' }).click();
  await page.click('#dGo');
  await expect(page.locator('#dErr')).toContainText('That game is running');
  expect(fs.existsSync(home('10000000/savedata_prospero/PPSA08338/sdimg_STELLAR0000'))).toBe(false);
});

test('deleting from the card needs a second click', async ({ page }) => {
  await startSim();
  await synced(7);
  await post('/api/mc/settings', { enabled: false });
  await page.goto(base);
  const game = page.locator('.game', { hasText: 'Returnal' });
  await game.locator('.btn.danger').click();
  await expect(game.locator('.btn.danger')).toHaveText('DELETE?');
  expect(fs.existsSync(card('10000000/PPSA02530/sdimg_RETURNAL0000'))).toBe(true);
  await game.locator('.btn.danger').click();
  await expect(page.locator('.game', { hasText: 'Returnal' })).toHaveCount(0);
  expect(fs.existsSync(card('10000000/PPSA02530'))).toBe(false);
  expect(fs.existsSync(home('10000000/savedata_prospero/PPSA02530/sdimg_RETURNAL0000'))).toBe(true);
  await page.click('button[data-tab=console]');
  await expect(page.locator('.game', { hasText: 'Returnal' }).locator('.chip')).toHaveText('NOT ON CARD');
});

test('without a drive nothing is written, and plugging one in starts the mirror', async ({ page }) => {
  await startSim(['--no-usb']);
  await page.goto(base);
  await expect(page.locator('#cardpill')).toContainText('no USB drive');
  await expect(page.locator('.empty')).toContainText('No USB drive connected');
  await page.click('button[data-tab=console]');
  await expect(page.locator('.chip').first()).toHaveText('NO CARD');
  await expect(page.locator('#backupAll')).toBeDisabled();
  expect(fs.existsSync(path.join(root, 'usb0', 'memcard'))).toBe(false);
  await shot(page, 'no-drive.png');
  await post('/sim/usb?n=0&on=1');
  await expect(page.locator('#cardpill')).toContainText('usb0', { timeout: 15000 });
  await synced(7);
  await page.click('button[data-tab=card]');
  await expect(page.locator('.game')).toHaveCount(5, { timeout: 30000 });
});

test('names from the console are shown as text, never as markup', async ({ page }) => {
  await startSim(['--no-usb']);
  fs.writeFileSync(home('1b2c3d4e/username.dat'), '<img src=x>');
  await post('/sim/usb?n=0&on=1');
  await synced(7);
  await page.goto(base);
  await expect(page.locator('.game', { hasText: 'Stellar Blade' })).toContainText('from <img src=x>');
  expect(await page.locator('.list img[src="x"], #term img').count()).toBe(0);
});

test('a second drive is left alone until it is made a card, then gets every save', async ({ page }) => {
  await startSim();
  await synced(7);
  await page.goto(base);
  await expect(page.locator('.slot')).toHaveCount(1); // one drive, one card
  await post('/sim/usb?n=1&on=1');
  const idle = page.locator('.slot.idle[data-usb="1"]');
  await expect(idle).toContainText('not a memory card', { timeout: 15000 });
  await page.waitForTimeout(6000); // a few watcher passes
  expect(fs.existsSync(path.join(root, 'usb1', 'memcard'))).toBe(false);
  await shot(page, 'second-drive-idle.png');

  await idle.getByRole('button', { name: 'USE AS CARD' }).click();
  await expect(page.locator('.slot[data-usb="all"]')).toContainText('2 cards', { timeout: 15000 });
  await expect(page.locator('#cardpill')).toContainText('2 · usb0, usb1');
  await expect.poll(async () => (await api('/api/mc/status')).drives.map(d => d.saves).join(), { timeout: 30000 }).toBe('7,7');
  for (const f of ['10000000/PPSA02530/sdimg_RETURNAL0000', '10000000/CUSA07408/sdimg_GOWSAVE0000', '10000000/CUSA07408/GOWSAVE0000.bin', '1b2c3d4e/PPSA08338/sdimg_STELLAR0000'])
    expect(fs.readFileSync(cardOn(1, f)).equals(fs.readFileSync(card(f)))).toBe(true);
  await expect(page.locator('#lCard')).toHaveText('ON 2 CARDS');
  await expect(page.locator('#sCard')).toHaveText('14');
  await expect(page.locator('#sSync')).toHaveText('7');
  await expect(page.locator('#watchText')).toContainText('2 cards');

  // every card: each copy says which card it is on
  const returnal = page.locator('.game', { hasText: 'Returnal' });
  await expect(returnal.locator('.srow')).toHaveCount(2);
  await expect(returnal.locator('.srow[data-usb="0"] .chip')).toHaveText('usb0');
  await expect(returnal.locator('.srow[data-usb="1"] .chip')).toHaveText('usb1');
  await shot(page, 'two-cards.png');
  // one card
  await page.locator('.slot[data-usb="1"]').click();
  await expect(page.locator('.srow')).toHaveCount(7);
  await expect(page.locator('.srow[data-usb="0"]')).toHaveCount(0);
  await expect(page.locator('#sCard')).toHaveText('7');
  await expect(page.locator('#lCard')).toHaveText('ON CARD');
  // console tab, every card: one mark per card
  await page.locator('.slot[data-usb="all"]').click();
  await page.click('button[data-tab=console]');
  await expect(page.locator('.chip[data-usb="0"][data-state="synced"]')).toHaveCount(7);
  await expect(page.locator('.chip[data-usb="1"][data-state="synced"]')).toHaveCount(7);
  await shot(page, 'two-cards-console.png');
});

test('with two cards, backup, import and delete act on the one that is selected', async ({ page }) => {
  await startSim();
  await fast();
  await post('/sim/usb?n=1&on=1');
  await post('/api/mc/card_init?usb=1');
  await cardsInUse(2);
  await expect.poll(async () => (await api('/api/mc/status')).drives.map(d => d.saves).join(), { timeout: 30000 }).toBe('7,7');
  await post('/api/mc/settings', { enabled: false });
  const rel = '1b2c3d4e/PPSA08338/sdimg_STELLAR0000';
  fs.writeFileSync(home('1b2c3d4e/savedata_prospero/PPSA08338/sdimg_STELLAR0000'), Buffer.concat([Buffer.from([2]), Buffer.from('only on the second card')]));

  await page.goto(base);
  await page.locator('.slot[data-usb="1"]').click();
  await page.click('button[data-tab=console]');
  await expect(page.locator('#backupAll')).toHaveText('BACK UP ALL TO USB1 ▸');
  const row = page.locator('.game', { hasText: 'Stellar Blade' }).locator('.srow');
  await expect(row.locator('.chip')).toHaveText('WAITING', { timeout: 30000 });
  await row.getByRole('button', { name: 'BACK UP' }).click();
  await expect.poll(() => fs.readFileSync(cardOn(1, rel)).toString('latin1'), { timeout: 30000 }).toContain('only on the second card');
  await expect(row.locator('.chip')).toHaveText('ON CARD', { timeout: 30000 });
  expect(fs.readFileSync(card(rel)).toString('latin1')).not.toContain('only on the second card');
  await expect(page.locator('#term')).toContainText('sdimg_STELLAR0000 -> usb1');
  // seen across both cards, the save is on one and behind on the other
  await page.locator('.slot[data-usb="all"]').click();
  await expect(row.locator('.chip[data-usb="0"]')).toHaveAttribute('data-state', 'pending');
  await expect(row.locator('.chip[data-usb="1"]')).toHaveAttribute('data-state', 'synced');

  // import the usb1 copy into the other profile
  await page.click('button[data-tab=card]');
  await page.locator('.game', { hasText: 'Stellar Blade' }).locator('.srow[data-usb="1"]').getByRole('button', { name: 'IMPORT' }).click();
  await expect(page.locator('#dMeta')).toContainText('usb1');
  await page.locator('#dUsers button', { hasText: 'heyde' }).click();
  await page.click('#dGo');
  await expect(page.locator('#veil')).toBeHidden({ timeout: 15000 });
  expect(fs.readFileSync(home('10000000/savedata_prospero/PPSA08338/sdimg_STELLAR0000')).toString('latin1')).toContain('only on the second card');

  // delete the usb1 copy only
  const copy = page.locator('.game', { hasText: 'Stellar Blade' }).locator('.srow[data-usb="1"]');
  await copy.locator('.btn.danger').click();
  await copy.locator('.btn.danger').click();
  await expect(page.locator('.game', { hasText: 'Stellar Blade' }).locator('.srow[data-usb="1"]')).toHaveCount(0);
  expect(fs.existsSync(cardOn(1, rel))).toBe(false);
  expect(fs.existsSync(card(rel))).toBe(true);
});

test('a card can be named, and keeps the name when it is plugged in again', async ({ page }) => {
  await startSim();
  await synced(7);
  await page.goto(base);
  page.once('dialog', d => d.accept('  Travel card '));
  await page.locator('.slot[data-usb="0"] .ren').click();
  await expect(page.locator('.slot[data-usb="0"] b')).toHaveText('Travel card');
  await expect(page.locator('#cardpill')).toContainText('Travel card');
  expect(fs.readFileSync(card('label.txt'), 'utf8')).toBe('Travel card');
  await post('/sim/usb?n=0&on=0');
  await expect(page.locator('#cardpill')).toContainText('no USB drive', { timeout: 15000 });
  await post('/sim/usb?n=0&on=1');
  await expect(page.locator('.slot[data-usb="0"] b')).toHaveText('Travel card', { timeout: 15000 });
  // the name is text, whatever it contains
  page.once('dialog', d => d.accept('<b>x</b>'));
  await page.locator('.slot[data-usb="0"] .ren').click();
  await expect(page.locator('.slot[data-usb="0"] b').first()).toHaveText('<b>x</b>');
});

test('"every drive" in the settings turns a new drive into a card by itself', async ({ page }) => {
  await startSim();
  await synced(7);
  await page.goto(base);
  await page.click('#set summary');
  await page.check('#cAll');
  await page.click('#saveSet');
  await post('/sim/usb?n=2&on=1');
  await expect(page.locator('.slot[data-usb="2"]')).toContainText('7 saves', { timeout: 30000 });
  expect(fs.existsSync(cardOn(2, '10000000/PPSA02530/sdimg_RETURNAL0000'))).toBe(true);
});

test('phone layout', async ({ page }) => {
  await startSim();
  await synced(7);
  await post('/sim/usb?n=1&on=1');
  await post('/api/mc/card_init?usb=1');
  await cardsInUse(2);
  await page.setViewportSize({ width: 390, height: 844 });
  await page.goto(base);
  await expect(page.locator('.game')).toHaveCount(5);
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
  await shot(page, 'card-phone.png');
});
