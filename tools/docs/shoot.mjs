import { writeFileSync, readFileSync, existsSync } from 'node:fs';
/* Screenshots of the web UI for documentation/ (dark theme, English).
 *   P4GW_URL=http://<gateway> P4GW_USER=<admin> P4GW_PASS=<password> PHASE=thread \
 *   node <browser-automation skill>/browser.mjs $P4GW_URL --script tools/docs/shoot.mjs
 * PHASE: thread (all pages, tooltips, dialogs; Thread border router role), zigbee (Zigbee coordinator role),
 * router (Zigbee router role). The account must be an admin without two-factor sign-in.
 * Then python3 tools/docs/build_docs.py. */
const OUT = new URL('../../documentation/screenshots/', import.meta.url).pathname;
const BASE = process.env.P4GW_URL || 'http://p4gw.local';
const PHASES = {
  thread: [['overview', 'role', 'thread/info', 'thread/topology', 'thread/diag', 'thread/settings', 'zigbee/backups', 'zigbee/network',
    'sound/live', 'sound/events', 'sound/log', 'sound/classes', 'sound/learn', 'ha/guide', 'ha/mqtt', 'ha/esphome', 'system/info',
    'system/lan', 'system/firmware', 'system/diag', 'system/netscan', 'system/settings', 'system/maint', 'system/notify',
    'system/users/list', 'system/users/roles', 'system/users/tokens', 'system/users/requests', 'system/users/policy', 'profile'], '', true],
  zigbee: [['overview', 'role', 'zigbee/info', 'zigbee/diag', 'zigbee/backups', 'zigbee/network', 'ha/guide'], '__zigbee', false],
  router: [['overview', 'zigbee/info', 'zigbee/diag'], '__router', false],
};
const [ROUTES, SUFFIX, EXTRAS] = PHASES[process.env.PHASE || 'thread'];
const shot = (page, name, opts = {}) => page.screenshot({ path: OUT + name + '.jpg', type: 'jpeg', quality: 82, ...opts });
export default async function run(page) {
  const meta = existsSync(OUT + 'index.json') ? JSON.parse(readFileSync(OUT + 'index.json', 'utf8')) : {};
  const done = [];
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.evaluate(() => { localStorage.setItem('theme', 'dark'); localStorage.setItem('lang', 'en'); localStorage.removeItem('tab'); });
  await page.goto(BASE + '/#overview');
  await page.reload();
  await page.waitForSelector('#lg-user', { timeout: 30000 });
  await page.waitForTimeout(800);
  if (EXTRAS) await shot(page, 'login');
  await page.fill('#lg-user', process.env.P4GW_USER);
  await page.fill('#lg-pass', process.env.P4GW_PASS);
  await page.click('#login button[type=submit]');
  await page.waitForSelector('#side-user a', { timeout: 30000 });
  await page.waitForTimeout(4000);
  for (const r of ROUTES) {
    await page.evaluate(rt => { location.hash = rt; }, r);
    await page.waitForTimeout(/topology|network|netscan/.test(r) ? 8000 : r.includes('lan') ? 9000 : 4000);
    const actual = await page.evaluate(() => location.hash.slice(1));
    if (actual !== r) { done.push(`${r} -> ${actual}`); continue; }
    await page.mouse.move(1430, 890);
    await page.evaluate(() => document.querySelectorAll('#main details').forEach(d => { d.open = true; }));
    await page.waitForTimeout(400);
    const base = r.replace(/\//g, '_') + SUFFIX;
    await shot(page, base, { fullPage: true });
    meta[r + SUFFIX] = { page: base, cards: [] };
    const count = (await page.$$('#main .card')).length;
    for (let i = 0; i < count; i++) {
      /* passive pages are redrawn by every status poll: find the card again and retry */
      for (let attempt = 0; attempt < 4; attempt++) {
        try {
          const el = (await page.$$('#main .card'))[i];
          if (!el) break;
          const title = await el.evaluate(e => e.querySelector('h3')?.innerText.trim() || '');
          const box = await el.boundingBox();
          if (!box || box.height < 40) break;
          const file = `${base}__${i + 1}`;
          await el.screenshot({ path: OUT + file + '.jpg', type: 'jpeg', quality: 82, timeout: 5000 });
          meta[r + SUFFIX].cards.push({ title, file });
          break;
        } catch (e) { await page.waitForTimeout(700); }
      }
    }
    done.push(r);
  }
  if (EXTRAS) {
    await page.evaluate(() => { location.hash = 'overview'; });
    await page.waitForTimeout(3000);
    for (const [id, name] of [['#hdr-eth', 'tip_lan'], ['#hdr-role', 'tip_role'], ['#hdr-peer', 'tip_peer'], ['#hdr-ha', 'tip_ha'], ['#hdr-inet', 'tip_internet'], ['#hdr-uptime', 'tip_uptime']]) {
      if (!await page.$(id) || !(await page.$eval(id, e => e.offsetParent !== null))) continue;
      await page.hover(id);
      await page.waitForTimeout(400);
      await shot(page, name, { clip: { x: 0, y: 0, width: 1440, height: 420 } });
      await page.mouse.move(700, 700);
      await page.waitForTimeout(200);
    }
    await page.click('#side-reboot');
    await page.waitForTimeout(500);
    await shot(page, 'dialog_restart');
    await page.click('#confirm button[value="no"]');
    await page.evaluate(() => { location.hash = 'system/users/list'; });
    await page.waitForSelector('#usr-new', { timeout: 10000 });
    await page.waitForTimeout(1500);
    await page.click('#usr-new');
    await page.waitForTimeout(500);
    await shot(page, 'dialog_new_user');
    await page.click('#fd-cancel');
    await page.waitForTimeout(300);
    await page.click('[data-usr-edit]');
    await page.waitForTimeout(500);
    await page.evaluate(() => document.querySelectorAll('#form-dlg details').forEach(d => { d.open = true; }));
    await shot(page, 'dialog_edit_user');
    await page.click('#fd-cancel');
    await page.evaluate(() => { location.hash = 'profile'; });
    await page.waitForSelector('#totp-on', { timeout: 10000 });
    await page.waitForTimeout(2500);
    await page.click('#totp-on');
    await page.waitForSelector('#form-dlg .qr svg', { timeout: 10000 });
    await page.waitForTimeout(400);
    await shot(page, 'dialog_totp');
    await page.click('#fd-cancel');
    await page.waitForTimeout(1000);
    if (await page.$('#tg-pair')) {
      await page.click('#tg-pair');
      await page.waitForTimeout(1500);
      await shot(page, 'dialog_telegram_pair');
      await page.click('#form-dlg button[type=submit]');
    }
  }
  writeFileSync(OUT + 'index.json', JSON.stringify(meta, null, 1));
  return done;
}
