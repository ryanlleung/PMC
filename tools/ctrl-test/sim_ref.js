// The reference numbers in test_ctrl.c, from the simulator itself: node sim_ref.js (Playwright,
// seeded Math.random as simulator/tests). The first block of numbers came from the same cases run in
// sequence (closed to 5, 4.5, 10, then 5 7 3 5, then helium off).
const { chromium } = require('playwright');
const page = async (b, fn) => { const p = await b.newPage();
  await p.addInitScript(() => { let a = 12345; Math.random = () => { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; }; });
  await p.goto('file://' + require('path').join(__dirname, '..', '..', 'simulator', 'index.html')); await p.waitForTimeout(300);
  const r = await p.evaluate(fn); await p.close(); return r; };
(async () => {
  const b = await chromium.launch({ executablePath: process.env.CHROMIUM });
  const pre = () => { paused = true; window.run = s => { const out = []; const n = Math.round(s / DT); for (let i = 0; i < n; i++) { simTick(); if (i % 100 === 99) out.push({ t, pv: PV, f: [...C.faults.keys()].join(',') }); } return out; };
    window.step = (tr, sp) => { let f = null, os = -1e9; tr.forEach(x => { if (f == null && Math.abs(x.pv - sp) <= P.band) f = x.t; if (f != null && x.t - f <= 200) os = Math.max(os, x.pv - sp); }); return +os.toFixed(3); };
    bootHomed(); run(120); };
  const res = {};
  res.steps = await page(b, `(${pre})(); setSP(5); toAuto(); const a = run(1500); setSP(4.5); run(900); setSP(10); const c = run(1200); ({ s5: step(a, 5), s10: step(c, 10) })`);
  res.pump = await page(b, `(${pre})(); setSP(5); toAuto(); run(1500); setPump(false); const t0 = t; const h = run(900).find(x => x.f); ({ f: h && h.f, after: h && Math.round(h.t - t0) })`);
  res.slip = await page(b, `(${pre})(); setSP(5); toAuto(); run(1500); BENCH.coupled = false; setSP(7); const t0 = t; const h = run(900).find(x => x.f); ({ f: h && h.f, after: h && Math.round(h.t - t0) })`);
  console.log(JSON.stringify(res)); await b.close();
})();
