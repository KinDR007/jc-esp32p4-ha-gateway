/* Menu state for role combinations and a restricted user, with /api/status and /api/auth/me rewritten in
 * the browser (the gateway is not touched).
 *   node <browser-automation skill>/browser.mjs http://127.0.0.1:8089/ --script check_menu.mjs */
const COMBOS = [['thread_br', 'zigbee_router'], ['zigbee_coordinator', 'none'], ['esphome', 'thread_br'], ['none', 'esphome'], ['thread_router', 'zigbee_coordinator']];
export default async function run(page) {
  let combo = COMBOS[0], restricted = false;
  await page.route('**/api/status', async route => {
    const r = await route.fetch(), j = await r.json();
    j.role.active = j.role.desired = combo[0];
    j.role.b.active = j.role.b.desired = combo[1];
    await route.fulfill({ response: r, json: j });
  });
  await page.route('**/api/auth/me', async route => {
    const r = await route.fetch(), j = await r.json();
    if (restricted) { j.role = 'user'; j.token = false; j.levels = { ...j.levels, 'system/settings': 0, 'system/maint': 1, 'ha/esphome': 0 }; }
    await route.fulfill({ response: r, json: j });
  });
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.evaluate(() => { localStorage.setItem('lang', 'cs'); });
  const out = {};
  const nav = () => page.evaluate(() => [...document.querySelectorAll('#side-nav > a')].map(a => a.innerText.trim() + (a.classList.contains('off') ? ' (šedě)' : '')).join(' | '));
  const subs = () => page.evaluate(() => [...document.querySelectorAll('#side-nav .subs a')].map(a => a.innerText.trim() + (a.classList.contains('off') ? ' (šedě)' : '')).join(' | '));
  for (const c of COMBOS) {
    combo = c;
    await page.reload();
    await page.waitForSelector('#side-user a', { timeout: 30000 });
    await page.waitForTimeout(2500);
    await page.evaluate(() => { location.hash = 'thread/info'; });
    await page.waitForTimeout(1500);
    const thread = await page.evaluate(() => location.hash);
    await page.evaluate(() => { location.hash = 'zigbee/info'; });
    await page.waitForTimeout(1500);
    out[c.join(' + ')] = { menu: await nav(), thread_link: thread, zigbee: await page.evaluate(() => location.hash), zigbee_subs: await subs() };
  }
  restricted = true; combo = COMBOS[0];
  await page.goto('http://127.0.0.1:8089/#system/lan');
  await page.reload();
  await page.waitForSelector('#side-user a', { timeout: 30000 });
  await page.waitForTimeout(2500);
  const cards = () => page.evaluate(() => [...document.querySelectorAll('#main .card h3')].map(h => h.innerText.trim()).join(' | '));
  out.restricted = { lan: await cards(), menu: await nav() };
  await page.evaluate(() => { location.hash = 'system/maint'; });
  await page.waitForTimeout(1500);
  out.restricted.maint = await cards();
  out.restricted.maint_restart_disabled = await page.evaluate(() => document.querySelector('#sys-reboot')?.disabled);
  await page.evaluate(() => { location.hash = 'ha/guide'; });
  await page.waitForTimeout(1500);
  out.restricted.ha_subs = await subs();
  await page.evaluate(() => { location.hash = 'ha/matter'; });
  await page.waitForTimeout(1500);
  out.restricted.ha_matter_goes_to = await page.evaluate(() => location.hash);
  return out;
}
