/* language, theme, gallery tabs and reveal on scroll; preferences kept in this browser only */
const root = document.documentElement;
const save = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* private mode */ } };
document.getElementById('lang').onclick = () => {
  root.dataset.lang = root.dataset.lang === 'cs' ? 'en' : 'cs';
  root.lang = root.dataset.lang;
  save('lang', root.dataset.lang);
};
document.getElementById('theme').onclick = () => {
  const dark = root.dataset.theme ? root.dataset.theme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
  root.dataset.theme = dark ? 'light' : 'dark';
  save('theme', root.dataset.theme);
};
root.lang = root.dataset.lang;

const shots = [...document.getElementById('shots').content.children];
document.getElementById('tabs').addEventListener('click', e => {
  const b = e.target.closest('button');
  if (!b) return;
  document.querySelectorAll('#tabs button').forEach(x => x.classList.toggle('on', x === b));
  const s = shots[+b.dataset.i];
  document.getElementById('shot').src = s.dataset.src;
  document.getElementById('cap').innerHTML = s.innerHTML;
});

const io = new IntersectionObserver(es => es.forEach(e => { if (e.isIntersecting) { e.target.classList.add('in'); io.unobserve(e.target); } }), { threshold: .12 });
document.querySelectorAll('.reveal').forEach(el => io.observe(el));
