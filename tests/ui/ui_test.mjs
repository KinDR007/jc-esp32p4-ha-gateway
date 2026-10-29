// Browser tests of the gateway web UI against a real board, in the same plain-HTTP context a user
// has (no clipboard API there, which API tests cannot catch). Test cases: tests/TESTCASES.md (UI01-UI24).
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
  'zigbee/backups', 'zigbee/network', 'sound/live', 'sound/events', 'sound/log', 'sound/classes', 'ha/guide', 'ha/mqtt', 'ha/esphome',
  'system/info', 'system/firmware', 'system/diag', 'system/netscan', 'system/settings', 'system/maint'];
// false when the menu has the page closed (it needs a role that is not active)
const tab = async route => {
  const [sec] = route.split('/');
  // the section link opens its first sub page and lists the others
  if (await page.$(`#side a[data-route="${sec}"][aria-disabled]`)) return false;
  await page.click(`#side a[data-route="${sec}"]`);
  if (route.includes('/')) {
    await page.waitForSelector(`#side .subs a[data-route="${route}"]`);
    if (await page.$(`#side a[data-route="${route}"][aria-disabled]`)) return false;
    await page.click(`#side a[data-route="${route}"]`);
  }
  await page.waitForTimeout(600);
  return true;
};
// pages that open only in their role
const needsRole = route => /^thread\//.test(route) ? !status.role.active.startsWith('thread')
  : /^zigbee\/(info|diag)$/.test(route) ? !status.role.active.startsWith('zigbee') : false;

await page.goto(base);
await page.waitForSelector('.card', { timeout: 20000 });
const status = await page.evaluate(() => fetch('/api/status', { headers: sessionStorage.getItem('auth') ? { Authorization: 'Basic ' + sessionStorage.getItem('auth') } : {} }).then(r => r.json()));
const hostId = status.hostname.replace(/-/g, '_');

await test('UI01', 'every tab renders without script errors or error toasts', async () => {
  for (const id of ROUTES) {
    await page.evaluate(() => { document.querySelector('#toast').textContent = ''; });
    if (!await tab(id)) {
      check(needsRole(id), `${id}: closed in the menu, its role is not active`);
      continue;
    }
    check(!needsRole(id), `${id}: open in the menu`);
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

await test('UI10', 'Home Assistant: header shows who uses the gateway, settings find HA and test the Matter Server', async () => {
  const s = await page.evaluate(() => fetch('/api/status').then(r => r.json()));
  await tab('overview');
  await page.waitForTimeout(1500);
  const peer = await page.$eval('#hdr-peer', e => ({ hidden: e.classList.contains('hidden'), text: e.innerText, title: e.title }));
  const expect = ['thread_br', 'zigbee_coordinator'].includes(s.role.active);
  check(peer.hidden === !expect, `header user pill ${expect ? 'shown' : 'hidden'} for ${s.role.active} (${peer.text}${peer.title ? ': ' + peer.title : ''})`);
  await tab('system/settings');
  await page.waitForFunction(() => !/Looking|Hledám/.test(document.querySelector('#ha-found')?.textContent || 'x'), null, { timeout: 15000 });
  const found = await page.$$('[data-ha-ip]');
  console.log(`  ${found.length} Home Assistant instance(s) found over mDNS`);
  if (found.length) {
    const before = await page.inputValue('#matter-url');
    await found[0].click();
    const url = await page.inputValue('#matter-url');
    check(/^ws:\/\/[\d.]+:5580\/ws$/.test(url), `"Use" fills ${url}`);
    await page.click('#matter-test');
    await page.waitForFunction(() => !/^…$/.test(document.querySelector('#matter-test-out').textContent), null, { timeout: 20000 });
    console.log(`  Matter Server test: ${await page.textContent('#matter-test-out')}`);
    await page.fill('#matter-url', before); // not saved: leave the setting as it was
  }
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI11', 'C6 flashing: progress in the header on every page, gone when it ends', async () => {
  let busy = true;
  await page.route(`${base}api/status`, async route => {
    const res = await route.fetch();
    const j = await res.json();
    if (busy) j.c6.flash = { busy: true, percent: 46, stage: 'writing 0x10000 (232 kB)', last_error: '', elapsed_s: 10 };
    await route.fulfill({ response: res, json: j });
  });
  await tab('sound/events');
  await page.waitForFunction(() => !document.querySelector('#hdr-flash').classList.contains('hidden'), null, { timeout: 15000 });
  check((await page.textContent('#hdr-flash')).includes('46 %'), `header pill: ${(await page.textContent('#hdr-flash')).trim()}`);
  check(await page.getAttribute('#hdr-flash', 'title') === 'writing 0x10000 (232 kB)', 'stage in the tooltip');
  const strip = await page.$eval('#flash-strip', e => [getComputedStyle(e).display, e.style.width]);
  check(strip[0] === 'block' && strip[1] === '46%', `strip under the header at ${strip[1]}`);
  check(!(await page.textContent('#main')).includes('Flash'), 'no banner on the page');
  check(await page.$eval('#busy-dlg', d => d.open && d.matches(':modal')), 'modal dialog blocks the page');
  check((await page.textContent('#busy-pct')) === '46 %' && /writing 232 kB at 0x10000/.test(await page.textContent('#busy-stage')), 'dialog shows the stage and 46 %');
  check(/about 1[0-3] s left/.test(await page.textContent('#busy-eta')), `estimate from the elapsed time: ${await page.textContent('#busy-eta')}`);
  await page.keyboard.press('Escape');
  check(await page.$eval('#busy-dlg', d => d.open), 'Escape does not close it');
  busy = false;
  await page.waitForFunction(() => document.querySelector('#hdr-flash').classList.contains('hidden'), null, { timeout: 15000 });
  check(await page.$eval('#flash-strip', e => getComputedStyle(e).display) === 'none', 'pill and strip gone after flashing');
  await page.waitForFunction(() => !document.querySelector('#busy-dlg').open, null, { timeout: 5000 });
  check(true, 'dialog closed after flashing');
  await page.unroute(`${base}api/status`);
});

await test('UI12', 'Zigbee network: Zigbee2MQTT instance, devices with details, map when one is stored', async () => {
  const d = await page.evaluate(() => fetch('/api/zigbee/net').then(r => r.json()));
  if (!d.base) {
    console.log('  skipped: no Zigbee2MQTT on the MQTT broker');
    return;
  }
  await tab('zigbee/network');
  await page.waitForSelector('#zdev table', { timeout: 20000 });
  const rows = await page.$$eval('#zdev tr[data-zrow]', r => r.length);
  check(rows === d.devices.length, `${rows} devices listed (${d.devices.length} in Zigbee2MQTT)`);
  check((await page.textContent('#z2m-info')).includes(d.base), `instance ${d.base} shown`);
  const row = await page.$('#zdev tr[data-zrow]:nth-child(3)');
  await row.scrollIntoViewIfNeeded();
  const bb = await row.boundingBox();
  await page.mouse.move(bb.x + 40, bb.y + bb.height / 2);
  await page.waitForTimeout(300);
  check(await page.$eval('#nodebox', e => getComputedStyle(e).display) === 'none', 'no details box over the device list');
  await page.mouse.move(2, 2);
  if (d.map) {
    check(await page.$$eval('#zmap svg .zn', n => n.length) === d.map.nodes.length, `map with ${d.map.nodes.length} nodes`);
    await hoverMapNode('#zmap svg .zn.router');
    check(/IEEE/.test(await page.textContent('#nodebox')), 'device details on hover over the map');
    check(await page.$$eval('#zmap svg.focus .zl.hi', l => l.length) > 0, 'links of the device highlighted');
    check(await boxAtMapCorner('#zmap svg'), 'details box in the top left corner of the map');
    check(await legendClear('#zmap'), 'legend covers neither the drawing nor the details box');
    await page.mouse.move(2, 2);
  }
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI13', 'Zigbee router: transmit power set from the page, Identify blinks the header icon', async () => {
  if (status.role.active !== 'zigbee_router') {
    console.log('  skipped: Zigbee router role is not active');
    return;
  }
  const before = (await page.evaluate(() => fetch('/api/status').then(r => r.json()))).role_status.router.tx_power;
  await tab('zigbee/info');
  await page.waitForSelector('#zr-txp');
  check(+(await page.inputValue('#zr-txp')) === before, `power field shows ${before} dBm`);
  const target = before === 10 ? 11 : 10;
  await page.fill('#zr-txp', String(target));
  await page.waitForTimeout(6000);
  check(+(await page.inputValue('#zr-txp')) === target, 'status poll keeps the value being typed');
  await page.click('#zr-txp-set');
  await page.waitForFunction(t => document.querySelector('#main').textContent.includes(t + ' dBm'), target, { timeout: 15000 });
  check(true, `router reports ${target} dBm`);
  await page.fill('#zr-txp', String(before));
  await page.click('#zr-txp-set');
  await page.click('#identify-btn');
  await page.waitForFunction(() => !document.querySelector('#hdr-ident').classList.contains('hidden'), null, { timeout: 5000 });
  const anim = await page.$eval('#hdr-ident .mdi', e => getComputedStyle(e).animationName);
  check(anim === 'blink', `header icon blinks (${anim})`);
  await page.waitForFunction(() => document.querySelector('#hdr-ident').classList.contains('hidden'), null, { timeout: 15000 });
  check(true, 'icon gone after 10 s');
  await tab('zigbee/network');
  await page.waitForSelector('#zdev table', { timeout: 20000 }).catch(() => {});
  const own = await page.$('#zdev tr[data-zrow] [data-zact="txpower"]');
  if (own) check(!!await page.$('#zdev [data-zact="identify"]'), 'own router row has power and Identify buttons');
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI14', 'Thread map: hovering a router highlights its links, the device list has no details box', async () => {
  if (!status.role.active.startsWith('thread')) {
    console.log('  skipped: no Thread role active');
    return;
  }
  await tab('thread/topology');
  await page.waitForSelector('#topo-map svg.tmap', { timeout: 30000 });
  await hoverMapNode('#topo-map svg .topo-node');
  check(/RLOC16/.test(await page.textContent('#nodebox')), 'router details on hover over the map');
  const hi = await page.$$eval('#topo-map svg.focus .topo-link.hi', l => l.length);
  const links = await page.$$eval('#topo-map svg .topo-link', l => l.length);
  check(links === 0 || hi > 0, `${hi} of ${links} links highlighted`);
  check(await boxAtMapCorner('#topo-map svg'), 'details box in the top left corner of the map');
  check(await legendClear('#topo-map'), 'legend covers neither the drawing nor the details box');
  await page.mouse.move(2, 2);
  check(!await page.$('#topo-map svg.focus'), 'highlight gone after leaving the node');
  const row = await page.$('#topo-devices tr:nth-child(2)');
  if (row) {
    await row.scrollIntoViewIfNeeded();
    const bb = await row.boundingBox();
    await page.mouse.move(bb.x + 40, bb.y + bb.height / 2);
    await page.waitForTimeout(300);
    check(await page.$eval('#nodebox', e => getComputedStyle(e).display) === 'none', 'no details box over the device list');
    await page.mouse.move(2, 2);
  }
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI15', 'info tips: one text per key, tables with the same column names explain their own data', async () => {
  const src = await page.evaluate(() => fetch('/').then(r => r.text()));
  const block = src.slice(src.indexOf('const TIPS = {'), src.indexOf('\n};', src.indexOf('const TIPS = {')));
  const keys = [...block.matchAll(/^  '([^']+)':/gm)].map(m => m[1]);
  const dup = keys.filter((k, i) => keys.indexOf(k) !== i);
  check(keys.length > 100 && dup.length === 0, `${keys.length} tip keys, no duplicates ${dup.join(', ')}`);
  const tips = async sel => page.$$eval(`${sel} th .tip`, t => t.map(x => x.dataset.tip));
  if (status.role.active.startsWith('thread')) {
    await tab('thread/topology');
    await page.waitForSelector('#topo-devices table', { timeout: 30000 });
    const t = await tips('#topo-devices');
    check(t.includes('Device firmware') && t.includes('Device Thread role') && !t.includes('Firmware'), `Thread devices: ${t.join(', ')}`);
  }
  const d = await page.evaluate(() => fetch('/api/zigbee/net').then(r => r.json()));
  if (d.base) {
    await tab('zigbee/network');
    await page.waitForSelector('#zdev table', { timeout: 20000 });
    const t = await tips('#zdev');
    check(['Zigbee model', 'Zigbee type', 'Zigbee availability'].every(k => t.includes(k)) && !t.some(k => ['Model', 'Type', 'State'].includes(k)), `Zigbee devices: ${t.join(', ')}`);
    const info = await page.$$eval('#z2m-info dt .tip', t => t.map(x => x.dataset.tip));
    check(info.includes('Z2M base topic') && info.includes('Z2M devices'), `Zigbee2MQTT instance: ${info.join(', ')}`);
  }
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI16', 'menu: pages of an inactive role are closed, a link to one opens a page that works', async () => {
  const off = ROUTES.filter(needsRole);
  if (!off.length) {
    console.log('  skipped: no role page closed in this role');
    return;
  }
  for (const r of off) {
    await page.evaluate(r => { location.hash = '#' + r; }, r);
    await page.waitForTimeout(800);
    const now = await page.evaluate(() => location.hash.slice(1));
    check(now !== r && !needsRole(now), `#${r} opens #${now}`);
  }
  for (const sec of new Set(off.map(r => r.split('/')[0]))) {
    const closed = await page.$eval(`#side a[data-route="${sec}"]`, a => a.hasAttribute('aria-disabled'));
    const allOff = ROUTES.filter(r => r.startsWith(sec + '/')).every(needsRole);
    check(closed === allOff, `${sec}: section ${closed ? 'closed' : 'open'}, ${allOff ? 'none' : 'some'} of its pages work${allOff ? '' : 's'}`);
  }
});

await test('UI17', 'network map: wheel and buttons zoom, home resets, L arranges by signal and back', async () => {
  let sel = null;
  if (status.role.active.startsWith('thread')) {
    await tab('thread/topology');
    sel = '#topo-map svg.topo';
  } else if ((await page.evaluate(() => fetch('/api/zigbee/net').then(r => r.json()))).map) {
    await tab('zigbee/network');
    sel = '#zmap svg.topo';
  }
  if (!sel) {
    console.log('  skipped: no map available');
    return;
  }
  await page.waitForSelector(sel + ' circle', { timeout: 60000 });
  const svg = await page.$(sel);
  await svg.scrollIntoViewIfNeeded();
  const vb = () => page.getAttribute(sel, 'viewBox');
  const base = await vb(), bb = await svg.boundingBox();
  await page.mouse.move(bb.x + bb.width / 2, bb.y + bb.height / 2);
  await page.mouse.wheel(0, -200);
  await page.waitForTimeout(300);
  const zoomed = await vb();
  check(+zoomed.split(' ')[2] < +base.split(' ')[2], `wheel zooms in (${base} -> ${zoomed})`);
  await page.click('.map-tools [data-mz="home"]');
  check(await vb() === base, 'home restores the whole map');
  await page.click('.map-tools [data-mz="in"]');
  check(+(await vb()).split(' ')[2] < +base.split(' ')[2], '+ zooms in');
  await page.click('.map-tools [data-mz="out"]');
  await page.click('.map-tools [data-mz="out"]');
  check(await vb() === base, '− zooms out no further than the whole map');
  const where = () => page.$$eval(`${sel} [data-node] circle, ${sel} [data-znode] circle`, c => c.slice(0, 6).map(e => e.getAttribute('cx')).join());
  const ring = await where();
  await page.mouse.move(2, 2);
  await page.keyboard.press('l');
  await page.waitForTimeout(400);
  check(await where() !== ring && await page.evaluate(() => localStorage.getItem('mapLayout')) === 'signal', 'L arranges by signal and remembers it');
  check(await legendClear(sel.split(' ')[0]), 'legend still clear of the drawing');
  await page.keyboard.press('l');
  await page.waitForTimeout(400);
  check(await where() === ring, 'L again restores the ring');
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI18', 'C6 temperature with its min and max like the P4', async () => {
  const tel = (await page.evaluate(() => fetch('/api/status').then(r => r.json()))).c6.telemetry;
  if (tel?.temp_c == null) {
    console.log('  skipped: the C6 firmware reports no temperature');
    return;
  }
  await tab('overview');
  const text = await page.$$eval('#main dt', dts => dts.filter(d => /C6 temperature|Temperature/.test(d.querySelector('.tip')?.dataset.tip || ''))
    .map(d => d.nextElementSibling.textContent));
  check(text.length === 2 && /min [\d.]+, max [\d.]+/.test(text[0]) && /min [\d.]+, max [\d.]+/.test(text[1]), `P4 and C6 with min/max: ${text.join(' | ')}`);
});

await test('UI19', 'Diagnostics: the log before the last restart downloads', async () => {
  await tab('system/diag');
  const [dl] = await Promise.all([page.waitForEvent('download', { timeout: 5000 }), page.click('#log-prev')]);
  const text = readFileSync(await dl.path(), 'utf8');
  check(dl.suggestedFilename().endsWith('-previous.log'), `file ${dl.suggestedFilename()}`);
  check(/^[IWE] \(/m.test(text), `log lines (${text.length} B)`);
});

await test('UI20', 'Settings backup: export downloads the settings, a foreign file and a cancelled import change nothing', async () => {
  await tab('system/settings');
  const [dl] = await Promise.all([page.waitForEvent('download', { timeout: 5000 }), page.click('#set-export')]);
  const file = JSON.parse(readFileSync(await dl.path(), 'utf8'));
  check(file.format === 'p4gw-settings' && !file.secrets && file.keys.role && !file.keys.password, `export without passwords, ${Object.keys(file.keys).length} keys`);
  await page.evaluate(() => { document.querySelector('#toast').textContent = ''; });
  await page.setInputFiles('#set-import', { name: 'x.json', mimeType: 'application/json', buffer: Buffer.from('{"format":"other"}') });
  await page.waitForFunction(() => document.querySelector('#toast').textContent.startsWith('Error'), null, { timeout: 3000 });
  check(true, `foreign file refused: ${await toastText()}`);
  await page.setInputFiles('#set-import', { name: 'ok.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(file)) });
  await page.waitForSelector('#confirm[open]');
  const text = await page.textContent('#confirm-text');
  await page.keyboard.press('Escape');
  check(text.includes(file.hostname) && !(await page.$('#busy-dlg[open]')), `confirmation names the source, cancel keeps the gateway running`);
});

await test('UI21', 'Zigbee backups: the automatic interval is stored, Back up now only in a Zigbee role', async () => {
  await page.goto(base + '#zigbee/backups');
  await page.waitForSelector('#zb-snap-days');
  const days = () => page.evaluate(() => fetch('/api/status').then(r => r.json()).then(s => s.c6.snapshot_days));
  const was = await days();
  await page.selectOption('#zb-snap-days', '1');
  await page.waitForTimeout(800);
  check(await days() === 1, 'daily stored');
  await page.selectOption('#zb-snap-days', String(was));
  await page.waitForTimeout(800);
  check(await days() === was, `restored to ${was}`);
  const role = await page.evaluate(() => fetch('/api/status').then(r => r.json()).then(s => s.role.active));
  check(await page.isDisabled('#zb-snap-now') === !role.startsWith('zigbee'), `Back up now ${role.startsWith('zigbee') ? 'enabled' : 'disabled'} in ${role}`);
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI22', 'Online update: the card saves the URL and shows why a check failed', async () => {
  await tab('system/firmware');
  await page.fill('#upd-url', `http://${host}/nope.json`);
  await page.click('#upd-save');
  await page.waitForFunction(() => document.querySelector('#upd-state')?.textContent.includes('HTTP 404'), null, { timeout: 25000 });
  check(true, `error shown: ${(await page.textContent('#upd-state')).trim()}`);
  check(await page.isDisabled('#upd-install'), 'Install disabled without an offer');
  await page.fill('#upd-url', '');
  await page.click('#upd-save');
  await page.waitForFunction(() => document.querySelector('#upd-state')?.textContent.includes('Off'), null, { timeout: 10000 });
  check(await page.isDisabled('#upd-check'), 'cleared: off, Check disabled');
});

await test('UI23', 'Learned sounds: the page loads, a foreign file is refused, a test recording shows its scores', async () => {
  await page.goto(base + '#sound/learn');
  await page.waitForFunction(() => document.querySelector('#ln-table') && !document.querySelector('#ln-table').textContent.includes('Loading'), null, { timeout: 10000 });
  check(/free slots|recognition off/.test(await page.textContent('#ln-state')), `state: ${(await page.textContent('#ln-state')).trim()}`);
  await page.setInputFiles('#ln-file', { name: 'x.wav', mimeType: 'audio/wav', buffer: Buffer.from('RIFF' + '\0'.repeat(100)) });
  await page.evaluate(() => { document.querySelector('#toast').textContent = ''; });
  await page.click('#ln-up-test');
  await page.waitForFunction(() => document.querySelector('#toast').textContent.startsWith('Error'), null, { timeout: 5000 });
  check(true, `foreign file refused: ${await toastText()}`);
  const rate = 16000, n = rate * 2, buf = Buffer.alloc(44 + n * 2);
  buf.write('RIFF', 0); buf.writeUInt32LE(36 + n * 2, 4); buf.write('WAVEfmt ', 8); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20); buf.writeUInt16LE(1, 22);
  buf.writeUInt32LE(rate, 24); buf.writeUInt32LE(rate * 2, 28); buf.writeUInt16LE(2, 32); buf.writeUInt16LE(16, 34); buf.write('data', 36); buf.writeUInt32LE(n * 2, 40);
  for (let i = 0; i < n; i++) buf.writeInt16LE(Math.round(8000 * Math.sin(2 * Math.PI * 1500 * i / rate)), 44 + 2 * i);
  await page.setInputFiles('#ln-file', { name: 'tone.wav', mimeType: 'audio/wav', buffer: buf });
  await page.click('#ln-up-test');
  await page.waitForFunction(() => /windows|oken/.test(document.querySelector('#ln-state').textContent), null, { timeout: 20000 });
  check(true, `test result: ${(await page.textContent('#ln-state')).trim()}`);
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

await test('UI24', 'MQTT: TLS options only for mqtts:// and wss://, the certificate only for an own CA, advanced options shown', async () => {
  await page.goto(base + '#ha/mqtt');
  await page.waitForSelector('#mq-uri');
  const shown = sel => page.evaluate(s => !document.querySelector(s).closest('.hidden'), sel);
  const uri = await page.inputValue('#mq-uri');
  await page.fill('#mq-uri', 'mqtt://10.0.0.1:1883');
  check(!(await shown('#mq-tls')), 'mqtt://: no TLS options');
  await page.fill('#mq-uri', 'mqtts://10.0.0.1:8883');
  check(await shown('#mq-tls') && !(await shown('#mq-ca')), 'mqtts://: certificate choice, public CA without a PEM field');
  await page.selectOption('#mq-tls', 'ca');
  check(await shown('#mq-ca'), 'own CA: PEM field');
  await page.fill('#mq-uri', 'wss://10.0.0.1:8884');
  check(await shown('#mq-tls'), 'wss://: TLS options');
  check(['#mq-cid', '#mq-ka', '#mq-base', '#mq-int', '#mq-disc'].every(async s => await page.$(s)), 'advanced options present');
  await page.fill('#mq-uri', uri);
  check(pageErrors.length === 0, `no uncaught script errors ${pageErrors.join('; ')}`);
});

// the legend sits beside or over an empty part of the map: no node, label or the details box under it
async function legendClear(box) {
  return page.evaluate(box => {
    const lg = document.querySelector(`${box} .map-legend`);
    if (!lg || lg.querySelectorAll('tr').length < 5) return false;
    const a = lg.getBoundingClientRect();
    const hit = r => r.right > a.left && r.left < a.right && r.bottom > a.top && r.top < a.bottom;
    return ![...document.querySelectorAll(`${box} svg.topo circle, ${box} svg.topo text`)].some(e => hit(e.getBoundingClientRect()))
      && !hit(document.querySelector('#nodebox').getBoundingClientRect());
  }, box);
}

// the details box sits 12 px from the top left corner of the visible map
async function boxAtMapCorner(sel) {
  return page.evaluate(sel => {
    const b = document.querySelector('#nodebox').getBoundingClientRect(), r = document.querySelector(sel).getBoundingClientRect();
    return Math.abs(b.left - (Math.max(r.left, 0) + 12)) < 1 && Math.abs(b.top - (Math.max(r.top, 52) + 12)) < 1;
  }, sel);
}

async function hoverMapNode(sel) {
  const node = await page.$(sel);
  await node.scrollIntoViewIfNeeded();
  const bb = await node.boundingBox();
  await page.mouse.move(bb.x + bb.width / 2, bb.y + bb.height / 2);
  await page.waitForTimeout(300);
}

await browser.close();
console.log('Summary: ' + results.map(r => `${r[0]} ${r[1]}`).join(', '));
process.exit(results.every(r => r[1] === 'PASS') ? 0 : 1);
