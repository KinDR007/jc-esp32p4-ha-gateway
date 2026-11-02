/* Walks every page of the menu and records its sidebar entry, cards (titles), buttons and internal links.
 *   OUT=<file.json> node <browser-automation skill>/browser.mjs http://127.0.0.1:8088/ --script inventory.mjs */
import { writeFileSync } from 'node:fs';
export default async function run(page) {
  await page.setViewportSize({ width: 1440, height: 900 });
  await page.evaluate(() => { localStorage.setItem('lang', 'cs'); localStorage.setItem('theme', 'dark'); });
  await page.reload();
  await page.waitForSelector('#side-user a', { timeout: 30000 });
  await page.waitForTimeout(3000);
  const routes = process.env.ROUTES.split(/\s+/);
  const pages = [];
  for (const r of routes) {
    await page.evaluate(rt => { location.hash = rt; }, r);
    await page.waitForTimeout(2500);
    pages.push(await page.evaluate(rt => ({
      route: rt,
      actual: location.hash.slice(1),
      cards: [...document.querySelectorAll('#main .card')].map(c => ({
        title: c.querySelector('h3')?.innerText.trim() || '',
        sub: [...c.querySelectorAll('h4, summary')].map(e => e.innerText.trim()).filter(Boolean),
        buttons: [...c.querySelectorAll('button, a.btn')].map(b => b.innerText.trim()).filter(Boolean),
        fields: [...c.querySelectorAll('label.field > span, label > span:first-child')].map(e => e.innerText.trim()).filter(Boolean),
        links: [...c.querySelectorAll('a[href^="#"]')].map(a => `${a.innerText.trim()} -> ${a.getAttribute('href')}`),
      })),
    }), r));
  }
  const nav = await page.evaluate(() => document.querySelector('nav, #side')?.innerText);
  writeFileSync(process.env.OUT, JSON.stringify({ nav, pages }, null, 1));
  return { routes: routes.length };
}
