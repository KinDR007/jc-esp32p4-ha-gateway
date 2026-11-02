/* Full-page screenshots of the given pages (Czech, dark theme) for the before/after comparison.
 *   ROUTES="overview role …" OUTDIR=<dir> node <browser-automation skill>/browser.mjs http://127.0.0.1:8088/ --script shoot.mjs */
export default async function run(page) {
  const out = process.env.OUTDIR.replace(/\/?$/, '/');
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.evaluate(() => { localStorage.setItem('lang', 'cs'); localStorage.setItem('theme', 'dark'); });
  await page.reload();
  await page.waitForSelector('#side-user a', { timeout: 30000 });
  await page.waitForTimeout(3000);
  const done = [];
  for (const r of process.env.ROUTES.split(/\s+/)) {
    await page.evaluate(rt => { location.hash = rt; }, r);
    await page.waitForTimeout(/topology|network|netscan|lan|radios/.test(r) ? 6000 : 3000);
    await page.mouse.move(1430, 890);
    const name = r.replace(/\//g, '_');
    await page.screenshot({ path: `${out}${name}.jpg`, type: 'jpeg', quality: 80, fullPage: !/diag$|log$|classes$/.test(r) });
    done.push(`${r} -> ${await page.evaluate(() => location.hash.slice(1))}`);
  }
  return done;
}
