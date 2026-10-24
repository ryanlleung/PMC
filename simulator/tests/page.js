// The page in Chromium (Playwright): loads, takes taps, homes the valve
// at full speed, answers a console command, and keeps settings over a
// reload.   node simulator/tests/page.js [shot.png]
const { chromium } = require('playwright');
const path = require('path');
(async () => {
  const b = await chromium.launch();
  const p = await b.newPage({ viewport: { width: 1400, height: 1000 } });
  const errs = [];
  p.on('pageerror', e => errs.push(String(e)));
  p.on('console', m => { if (m.type() === 'error') errs.push(m.text()); });
  await p.goto('file://' + path.join(__dirname, '..', 'index.html'));
  await p.waitForTimeout(1500);
  const box = await p.locator('#screen').boundingBox(), k = box.width / 480;
  const tap = async (x, y) => { await p.mouse.click(box.x + x * k, box.y + y * k); await p.waitForTimeout(400); };
  await tap(415, 72);    // Home valve
  await tap(346, 147);   // Home
  await p.selectOption('#speed', '600');
  await p.waitForFunction(() => document.getElementById('console').textContent.includes('Homed in'), null, { timeout: 30000 });
  await p.selectOption('#speed', '1');
  await p.fill('#cmd', 'valve?');
  await p.press('#cmd', 'Enter');
  await p.waitForTimeout(500);
  const text = await p.locator('#console').textContent();
  const ok1 = /VALVE mode MANUAL, calibrated 1/.test(text);
  await p.fill('#cmd', 'PID KP 3'); await p.press('#cmd', 'Enter');
  await p.fill('#cmd', 'CFG SAVE'); await p.press('#cmd', 'Enter');
  await p.waitForTimeout(1500);
  await p.reload(); await p.waitForTimeout(1500);
  await p.fill('#cmd', 'PID?'); await p.press('#cmd', 'Enter'); await p.waitForTimeout(500);
  const ok2 = /PID KP 3\.00/.test(await p.locator('#console').textContent());
  if (process.argv[2]) await p.screenshot({ path: process.argv[2], fullPage: true });
  await b.close();
  console.log((ok1 ? 'ok   ' : 'FAIL ') + 'homed from the screen and VALVE? answered');
  console.log((ok2 ? 'ok   ' : 'FAIL ') + 'settings kept over a reload');
  console.log((errs.length ? 'FAIL ' : 'ok   ') + 'no page errors' + (errs.length ? ': ' + errs.join('; ') : ''));
  process.exit(ok1 && ok2 && !errs.length ? 0 : 1);
})();
