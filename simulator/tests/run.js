// Usage: node tests/run.js tests/<script>.js [out-dir]
// Needs Playwright (npm i -g playwright). Opens simulator/index.html, switches the screen to 1:1 (480 x 272) and runs the
// script with helpers. Screenshots are native size PNGs in out-dir (default tests/shots).
const path = require('path'), fs = require('fs');
const { chromium } = require('playwright');
(async () => {
  const out = path.resolve(process.argv[3] || path.join(__dirname, 'shots')); fs.mkdirSync(out, { recursive: true });
  const exe = process.env.CHROMIUM || (fs.existsSync('/opt/pw-browsers') ? (fs.readdirSync('/opt/pw-browsers').filter(d => d.startsWith('chromium-')).map(d => '/opt/pw-browsers/' + d + '/chrome-linux/chrome').find(fs.existsSync)) : undefined);
  const b = await chromium.launch(exe ? { executablePath: exe } : {});
  const p = await b.newPage({ viewport: { width: 1100, height: 900 }, deviceScaleFactor: 1 });
  const errs = []; p.on('pageerror', e => errs.push(e.message)); p.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  // seeded noise so runs repeat (the physics model adds sensor noise with Math.random)
  await p.addInitScript(() => { let a = 12345; Math.random = () => { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; }; });
  await p.goto('file://' + path.join(__dirname, '..', 'index.html'));
  await p.waitForTimeout(400);
  await p.evaluate(() => { nativeSize = true; fit(); paused = true; });
  const h = require('./helpers.js')(p, out);
  let fails = 0; h.ok = (c, m) => { if (!c) fails++; console.log((c ? 'PASS ' : 'FAIL ') + m); };
  await require(path.resolve(process.argv[2]))(p, h);
  if (errs.length) { fails++; console.log('PAGE ERRORS ' + JSON.stringify(errs)); }
  console.log(fails ? fails + ' failed' : 'all passed');
  await b.close();
  process.exit(fails ? 1 : 0);
})();
