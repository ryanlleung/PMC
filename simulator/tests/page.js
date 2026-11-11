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
  // Every start is OFF (0.15.1): a double tap turns it on, then the logo.
  await p.mouse.click(box.x + 240 * k, box.y + 136 * k); await p.waitForTimeout(200);
  await tap(240, 136);
  await p.waitForTimeout(3000);
  await tap(269, 184);   // Home valve (the Valve tile)
  await tap(346, 147);   // Home
  await p.selectOption('#speed', '600');
  // The board sends nothing unasked: ask MODE? until homing is done.
  let homed = false;
  for (let i = 0; i < 60 && !homed; i++) {
    await p.fill('#cmd', 'mode?'); await p.press('#cmd', 'Enter'); await p.waitForTimeout(500);
    homed = (await p.locator('#console').textContent()).includes('MODE MANUAL');
  }
  await p.selectOption('#speed', '1');
  await p.fill('#cmd', 'valve?');
  await p.press('#cmd', 'Enter');
  await p.waitForTimeout(500);
  const text = await p.locator('#console').textContent();
  const ok1 = homed && /> VALVE\?\nVALVE \d+\.\d\d %/.test(text);
  // Save settings with KP 3.00 (the test hook, as Review and save), reload, read it back.
  await p.evaluate(() => x.sim_save(9, 300));
  await p.waitForTimeout(1500);
  await p.reload(); await p.waitForTimeout(1500);
  const ok2 = (await p.evaluate(() => x.sim_setting(9))) === 300;
  if (process.argv[2]) await p.screenshot({ path: process.argv[2], fullPage: true });
  await b.close();
  console.log((ok1 ? 'ok   ' : 'FAIL ') + 'homed from the screen and VALVE? answered');
  console.log((ok2 ? 'ok   ' : 'FAIL ') + 'settings kept over a reload');
  console.log((errs.length ? 'FAIL ' : 'ok   ') + 'no page errors' + (errs.length ? ': ' + errs.join('; ') : ''));
  process.exit(ok1 && ok2 && !errs.length ? 0 : 1);
})();
