/* The left menu with each section opened in turn, one PNG per section (Czech, dark theme); compare.py
 * puts them side by side.
 *   SECTIONS="overview radios/roles …" OUTDIR=<dir> node <browser-automation skill>/browser.mjs http://127.0.0.1:8088/ --script menu_shot.mjs */
export default async function run(page) {
  const out = process.env.OUTDIR.replace(/\/?$/, '/');
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.evaluate(() => { localStorage.setItem('lang', 'cs'); localStorage.setItem('theme', 'dark'); });
  await page.reload();
  await page.waitForSelector('#side-user a', { timeout: 30000 });
  await page.waitForTimeout(2500);
  const done = [];
  for (const [i, r] of process.env.SECTIONS.split(/\s+/).entries()) {
    await page.evaluate(rt => { location.hash = rt; }, r);
    await page.waitForTimeout(1200);
    await page.mouse.move(1430, 890);
    const el = await page.$('#side-nav');
    await el.screenshot({ path: `${out}menu_${String(i).padStart(2, '0')}.png` });
    done.push(r);
  }
  return done;
}
