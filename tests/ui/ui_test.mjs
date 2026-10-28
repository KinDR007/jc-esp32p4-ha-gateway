// Browser tests of the gateway web UI against a real board, in the same plain-HTTP context a user
// has (no clipboard API there, which API tests cannot catch). Test cases: tests/TESTCASES.md (UI01-UI09).
//
//   npm --prefix tests/ui ci
//   node tests/ui/ui_test.mjs --host 192.168.88.243 [--password pw] [--html firmware/gateway/web/index.html]
//
// --html serves the page from a local file while the API stays on the board (test a UI change
// before flashing it, or check that a test fails on the old page).
import { readFileSync } from 'node:fs';
import { chromium } from 'playwright-core';

const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) => {
  if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1]?.startsWith('--') || all[i + 1] === undefined ? true : all[i + 1]]);
  return acc;
}, []));
const host = args.host || 'p4gw.local';
const base = `http://${host}/`;
const results = [];
const pageErrors = [];

function check(cond, msg) {
  if (!cond) throw new Error(msg);
  console.log(`  ok: ${msg}`);
}

async function test(id, title, fn) {
  console.log(`${id} ${title}`);
  try {
    await fn();
    results.push([id, 'PASS']);
    console.log(`${id} PASS\n`);
  } catch (e) {
    results.push([id, 'FAIL', e.message]);
    console.log(`${id} FAIL ${e.message}\n`);
  }
}

// A page served via page.route counts as public address space; Chrome's private network access
// checks would then block its API calls to the board.
const pnaOff = ['--disable-features=BlockInsecurePrivateNetworkRequests,PrivateNetworkAccessSendPreflights,LocalNetworkAccessChecks'];
const browser = await chromium.launch({ channel: 'chrome', headless: true, args: args.html ? pnaOff : [] });
// English locale: the UI picks Czech for a Czech browser and the error checks match English text.
const context = await browser.newContext({ locale: 'en-US', ...(args.password ? { httpCredentials: { username: 'admin', password: args.password } } : {}) });
const page = await context.newPage();
page.on('pageerror', e => pageErrors.push(e.message));
if (args.html && args.html !== true) {
  const html = readFileSync(args.html, 'utf8');
  await page.route(base, route => route.fulfill({ status: 200, contentType: 'text/html; charset=utf-8', body: html }));
}
if (args.password) {
  await page.addInitScript(pw => { try { sessionStorage.setItem('auth', btoa('admin:' + pw)); } catch (e) { /* ignore */ } }, args.password);
}

const toastText = () => page.textContent('#toast');
// every page of the left menu (section/sub page)
const ROUTES = ['overview', 'role', 'thread/info', 'thread/topology', 'thread/diag', 'thread/settings', 'zigbee/info', 'zigbee/diag',
  'zigbee/backups', 'sound/live', 'sound/events', 'sound/log', 'sound/classes', 'ha/guide', 'ha/mqtt', 'ha/esphome',
  'system/info', 'system/firmware', 'system/diag', 'system/netscan', 'system/settings', 'system/maint'];
const tab = async route => {
  const [sec] = route.split('/');
  // the section link opens its first sub page and lists the others
  await page.click(`#side a[data-route="${sec}"]`);
  if (route.includes('/')) await page.click(`#side a[data-route="${route}"]`);
  await page.waitForTimeout(600);
};

await page.goto(base);
await page.waitForSelector('.card', { timeout: 20000 });
const status = await page.evaluate(() => fetch('/api/status', { headers: sessionStorage.getItem('auth') ? { Authorization: 'Basic ' + sessionStorage.getItem('auth') } : {} }).then(r => r.json()));
const hostId = status.hostname.replace(/-/g, '_');

await test('UI01', 'every tab renders without script errors or error toasts', async () => {
  for (const id of ROUTES) {
    await page.evaluate(() => { document.querySelector('#toast').textContent = ''; });
    await tab(id);
    await page.waitForTimeout(1200);
    const t = await toastText();
    check(!t.startsWith('Error') && await page.evaluate(r => location.hash === '#' + r, id), `${id}: opened, no error toast${t ? ` (${t})` : ''}`);
  }
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI02', 'Sound: every "HA automation" button shows the automation for its entity', async () => {
  await tab('sound/events');
  await page.waitForSelector('button[data-ev-yaml]', { timeout: 20000 });
  const n = await page.$$eval('button[data-ev-yaml]', b => b.length);
  check(n > 0, `${n} events listed`);
  for (let i = 0; i < n; i++) {
    await page.evaluate(() => { document.querySelector('#toast').textContent = ''; });
    await page.click(`button[data-ev-yaml="${i}"]`);
    await page.waitForTimeout(400);
    const t = await toastText();
    if (t.startsWith('Error')) throw new Error(`event ${i}: ${t}`);
    const open = await page.evaluate(() => document.querySelector('#copy-dlg')?.open === true);
    if (!open) throw new Error(`event ${i}: automation dialog did not open`);
    const yaml = await page.inputValue('#copy-text');
    if (!yaml.includes('triggers:') || !new RegExp(`(event|binary_sensor)\\.${hostId}_snd_[a-z0-9_]+`).test(yaml)) {
      throw new Error(`event ${i}: unexpected YAML ${yaml.slice(0, 80)}`);
    }
    await page.click('#copy-dlg button[value=close]');
  }
  check(true, `all ${n} automation dialogs open with a valid entity id and no error`);
});

await test('UI03', 'Sound: event editor opens with the event and cancels without changes', async () => {
  const before = await page.evaluate(() => fetch('/api/sound/events').then(r => r.json()).then(c => JSON.stringify(c.events.map(e => [e.id, e.threshold]))));
  await page.click('button[data-ev-edit="0"]');
  await page.waitForTimeout(300);
  check(await page.evaluate(() => document.querySelector('#ev-dlg').open), 'editor open');
  check((await page.inputValue('#ev-id')).length > 0, 'event id filled');
  await page.click('#ev-dlg button[value=cancel]');
  await page.waitForTimeout(800);
  const after = await page.evaluate(() => fetch('/api/sound/events').then(r => r.json()).then(c => JSON.stringify(c.events.map(e => [e.id, e.threshold]))));
  check(before === after, 'configuration unchanged after cancel');
});

await test('UI04', 'Sound: search filters the tree of all sounds', async () => {
  await tab('sound/classes');
  await page.waitForSelector('.cls-row', { state: 'attached', timeout: 20000 });
  await page.fill('#cls-search', 'aircraft');
  await page.waitForTimeout(400);
  const visible = await page.$$eval('.cls-row', rs => rs.filter(r => r.style.display !== 'none').length);
  check(visible >= 1 && visible <= 10, `${visible} matching sounds shown`);
  await page.fill('#cls-search', '');
});

await test('UI05', 'Thread: "Copy TLVs" shows the dataset (border router or router role only)', async () => {
  if (!status.role.active.startsWith('thread')) {
    console.log('  skipped: no Thread role active');
    return;
  }
  await tab('thread/settings');
  await page.waitForSelector('#ds-copy', { timeout: 10000 });
  await page.evaluate(() => { document.querySelector('#toast').textContent = ''; });
  await page.click('#ds-copy');
  await page.waitForTimeout(1500);
  const t = await toastText();
  check(!t.startsWith('Error'), `no error toast${t ? ` (${t})` : ''}`);
  const tlvs = await page.inputValue('#copy-text');
  check(/^[0-9a-f]{40,}$/.test(tlvs), `dataset TLVs shown (${tlvs.length} hex chars)`);
  await page.click('#copy-dlg button[value=close]');
});

await test('UI06', 'Language: Czech translates every tab, keeps sound names, English restores the original', async () => {
  await page.selectOption('#pref-lang', 'cs');
  await page.waitForTimeout(800);
  check(await page.getAttribute('html', 'lang') === 'cs', 'html lang = cs');
  check((await page.textContent('#side a[data-route=overview]')).trim() === 'Přehled', 'Overview menu item reads "Přehled"');
  for (const id of ROUTES) {
    await page.evaluate(() => { document.querySelector('#toast').textContent = ''; });
    await tab(id);
    await page.waitForTimeout(1200);
    const t = await toastText();
    check(!t.startsWith('Error') && !t.startsWith('Chyba'), `${id}: no error toast${t ? ` (${t})` : ''}`);
    if (id === 'sound/classes') {
      await page.waitForSelector('.cls-row', { state: 'attached', timeout: 20000 });
      check(await page.$$eval('.cls-row span[data-notr]', s => s.some(e => e.textContent === 'Speech')), 'sound class names stay untranslated');
    }
    if (id === 'sound/events') {
      await page.waitForSelector('#ev-table b', { timeout: 20000 });
      check((await page.textContent('#main')).includes('Události posílané do Home Assistant'), 'sound events page translated');
      const ev = (await page.evaluate(() => fetch('/api/sound/events').then(r => r.json()))).events.find(e => e.name_cs);
      if (ev) check(await page.$$eval('#ev-table b', (b, n) => b.some(x => x.textContent === n), ev.name_cs), `event shown by its Czech name (${ev.name_cs})`);
      if (ev) {
        const label = await page.evaluate(([i, n]) => typeof heardName === 'function' ? heardName({ index: i, name: n }) : '', [ev.classes[0], 'class']);
        check(label.includes(ev.name_cs), `"Heard now" labels a class of that event in Czech (${label.replace(/<[^>]+>/g, ' ')})`);
      }
    }
  }
  // every menu item and sub page has a Czech label (names of standards stay as they are)
  const same = ['Role', 'Thread', 'Zigbee', 'Home Assistant', 'MQTT', 'ESPHome', 'Firmware'];
  const english = [];
  for (const sec of ['overview', 'role', 'thread', 'zigbee', 'sound', 'ha', 'system']) {
    await page.click(`#side a[data-route="${sec}"]`);
    await page.waitForTimeout(300);
    for (const [route, text] of await page.$$eval('#side a', a => a.map(x => [x.dataset.route, x.textContent.trim()]))) {
      const en = await page.evaluate(r => { const [s, sub] = r.split('/'); const n = NAV.find(x => x.id === s); return sub ? n.subs.find(x => x.id === sub).label : n.label; }, route);
      if (text === en && !same.includes(en)) english.push(en);
    }
  }
  check(english.length === 0, `menu fully translated${english.length ? ' (missing: ' + [...new Set(english)].join(', ') + ')' : ''}`);
  await page.reload();
  await page.waitForSelector('.card', { timeout: 20000 });
  check(await page.inputValue('#pref-lang') === 'cs', 'language kept after reload');
  await page.selectOption('#pref-lang', 'en');
  await page.waitForTimeout(800);
  check((await page.textContent('#side a[data-route=overview]')).trim() === 'Overview', 'English restores "Overview"');
  check(!/[ěščřžůť]/.test(await page.innerText('body')), 'no Czech text left on the page');
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI07', 'Theme: light, dark and system switch the page colours and persist', async () => {
  const bg = () => page.evaluate(() => getComputedStyle(document.body).backgroundColor);
  await page.selectOption('#pref-theme', 'light');
  const light = await bg();
  check(await page.getAttribute('html', 'data-theme') === 'light', `light: data-theme=light (${light})`);
  await page.selectOption('#pref-theme', 'dark');
  const dark = await bg();
  check(await page.getAttribute('html', 'data-theme') === 'dark' && dark !== light, `dark: different background (${dark})`);
  await page.reload();
  await page.waitForSelector('.card', { timeout: 20000 });
  check(await bg() === dark && await page.inputValue('#pref-theme') === 'dark', 'dark kept after reload');
  await page.emulateMedia({ colorScheme: 'light' });
  await page.selectOption('#pref-theme', 'system');
  check(await page.getAttribute('html', 'data-theme') === null && await bg() === light, 'system + light OS: no attribute, light background');
  await page.emulateMedia({ colorScheme: 'dark' });
  check(await bg() === dark, 'system + dark OS: dark background');
  await page.emulateMedia({ colorScheme: null });
});

await test('UI08', 'Navigation: left menu with sub pages, deep links, mobile drawer, explanation tooltips', async () => {
  await page.setViewportSize({ width: 1400, height: 900 });
  await tab('thread/info');
  const subs = await page.$$eval('#side .subs a', a => a.map(x => x.dataset.route));
  check(subs.join() === 'thread/info,thread/topology,thread/diag,thread/settings', `Thread sub pages listed (${subs.join(', ')})`);
  check(await page.isVisible('#side') && !(await page.isVisible('#menu-btn')), 'desktop: menu on the left, no menu button');
  check(await page.$$eval('#side svg.mdi path', p => p.every(x => (x.getAttribute('d') || '').length > 10)), 'every menu icon has an SVG path');
  await page.goto(base + '#system/diag');
  await page.waitForSelector('#log', { timeout: 20000 });
  check(await page.$eval('#side a.cur', a => a.dataset.route) === 'system/diag', 'deep link #system/diag opens that page after a reload');
  await page.goto(base + '#tools');
  await page.waitForSelector('#log', { timeout: 20000 });
  check(await page.evaluate(() => location.hash) === '#system/diag', 'old #tools link lands on System → Diagnostics');
  await page.goBack();
  await page.waitForTimeout(800);
  await tab('overview');
  const t = await page.$('#main .tip[data-tip="Temperature"]');
  check(!!t, 'Temperature has an explanation icon');
  await t.hover();
  await page.waitForTimeout(200);
  check(await page.isVisible('#tipbox') && (await page.textContent('#tipbox')).includes('P4'), `hover shows the explanation (${(await page.textContent('#tipbox')).slice(0, 40)}…)`);
  await page.setViewportSize({ width: 390, height: 800 });
  await page.waitForTimeout(400);
  check(await page.isVisible('#menu-btn'), 'phone: menu button shown');
  const x = () => page.$eval('#side', el => el.getBoundingClientRect().right);
  check(await x() <= 0, 'phone: menu hidden');
  await page.click('#menu-btn');
  await page.waitForTimeout(400);
  check(await x() > 100, 'menu button opens the drawer');
  await page.click('#side a[data-route="role"]');
  await page.waitForTimeout(600);
  check(await x() <= 0 && await page.evaluate(() => location.hash) === '#role', 'choosing a page closes the drawer');
  await page.setViewportSize({ width: 1280, height: 720 });
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI09', 'Network surroundings: the last scan shows channels, spectrum and a recommendation', async () => {
  const d = await page.evaluate(() => fetch('/api/net/scan').then(r => r.json()));
  if (!d.last) {
    console.log('  skipped: no scan stored on the gateway');
    return;
  }
  await tab('system/netscan');
  await page.waitForSelector('#ns-channels table', { timeout: 20000 });
  check(await page.$$eval('#ns-channels tr', r => r.length) === 17, 'all 16 channels (11-26) rated');
  check(await page.$$eval('#ns-spectrum svg rect', r => r.length) === 16, 'spectrum shows the 16 channels');
  check(/\d/.test(await page.textContent('#ns-reco')), `recommendation shown (${(await page.textContent('#ns-reco')).replace(/\s+/g, ' ').slice(0, 60)}…)`);
  if (d.last.wifi) {
    const rows = await page.$$eval('#ns-wifi tr', r => r.length - 1);
    const unique = new Set(d.last.wifi.map(a => a.bssid)).size;
    check(rows === unique, `Wi-Fi table: one row per BSSID (${rows})`);
  }
  check(await page.isDisabled('#ns-quick') === !d.quick_available, 'quick scan offered only when the role can run it');
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await browser.close();
console.log('Summary: ' + results.map(r => `${r[0]} ${r[1]}`).join(', '));
process.exit(results.every(r => r[1] === 'PASS') ? 0 : 1);
