// WP023 proposal acceptance checks (CHANGES.md S1 to S12). Run: node tests/run.js tests/wp023.js
module.exports = async (p, h) => {
  const { ok } = h, ev = f => p.evaluate(f);
  await ev(() => { localStorage.clear(); SAVED = defaultCfg(); loadActive(SAVED); bootHomed(); setHe(true); setPump(true); show('ctrl'); });
  // colours that must not appear in normal states (old green and amber)
  const banned = () => ev(() => { const bad = ['rgb(46, 160, 67)', 'rgb(214, 130, 0)', 'rgb(154, 93, 0)', 'rgb(255, 243, 221)'];
    const hits = []; document.querySelectorAll('#dev *').forEach(e => { const cs = getComputedStyle(e); if (cs.display === 'none') return;
      [cs.color, cs.backgroundColor].forEach(c => { if (bad.includes(c) && (e.textContent.trim() || cs.backgroundColor === c)) hits.push(c); }); });
    return [...new Set(hits)]; });
  let r;
  // S10, S3: Target max 50; keypad refuses a second decimal; serial target rounded to 0.1
  r = await ev(() => { keypadSP(); const info = overlay.textContent; closeOverlay(); return { max: P.spMax, info }; });
  ok(r.max === 50 && /Allowed 0\.2 to 50/.test(r.info), 'S10 Target max 50, keypad shows the range: ' + r.info.slice(0, 60));
  r = await ev(() => { command('CTRL SP 15.05'); return C.SP; });
  ok(r === 15.1 || r === 15, 'S3 serial target rounded to 0.1 mbar: ' + r);
  // S1, S3, S4: AUTO, step, readings 1 dp, no green or amber
  await ev(() => { for (let i = 0; i < 3000; i++) simTick(); setSP(5); toAuto(); for (let i = 0; i < 60000; i++) simTick(); setSP(8); for (let i = 0; i < 40000; i++) simTick(); updateUI(); });
  r = await ev(() => document.querySelector('#dev').textContent.match(/\b\d+\.\d+\b/g).slice(0, 4));
  ok(r.every(x => /^\d+\.\d$/.test(x)), 'S3 home readings at 0.1: ' + r.join(' '));
  r = await banned(); ok(r.length === 0, 'S4 no green or amber in AUTO: ' + r.join(','));
  // S1: the step is visible in the sparkline (dark pen pixels on more than one row band)
  r = await ev(() => { const cv = document.querySelector('#dev canvas'); const d = cv.getContext('2d').getImageData(0, 0, cv.width, cv.height).data; const rows = new Set();
    for (let y = 0; y < cv.height; y++) for (let x = 0; x < cv.width / 3; x++) { const i = (y * cv.width + x) * 4; if (d[i] < 80 && d[i + 2] > 90) rows.add(y >> 3); } return rows.size; });
  ok(r >= 2, 'S1 earlier target step visible on the home sparkline (pen spans ' + r + ' row bands)');
  // S5, S6: fault unacknowledged blinks; tap acknowledges; latch stays
  r = await ev(() => { BENCH.druck = false; for (let i = 0; i < 300; i++) simTick(); show('ctrl'); updateUI(); const sb = [...document.querySelectorAll('#dev .card')].find(e => e.classList.contains('k-fault'));
    const tile = document.querySelector('#dev .tile-alm'); return { strip: !!sb && sb.classList.contains('unack'), hdr: G.h.classList.contains('unack'), tile: !!tile, ua: unacked() }; });
  ok(r.strip && r.hdr && r.tile && r.ua, 'S5 S6 new fault: strip, header and pressure tile blink');
  r = await ev(() => { const sb = [...document.querySelectorAll('#dev .card')].find(e => e.classList.contains('k-fault')); sb.dispatchEvent(new PointerEvent('pointerdown', { bubbles: true })); closeOverlay(); updateUI();
    return { ua: unacked(), latched: C.faults.has('F-01'), blink: !!document.querySelector('#dev .unack') }; });
  ok(!r.ua && r.latched && !r.blink, 'S5 tap acknowledges, blink stops, latch stays');
  await ev(() => { BENCH.druck = true; for (let i = 0; i < 300; i++) simTick(); ack(); updateUI(); });
  // S4: not referenced is a prompt
  r = await ev(() => { document.getElementById('reboot').click(); show('ctrl'); updateUI(); return [...document.querySelectorAll('#dev .card')].some(e => e.classList.contains('k-prompt') && /Reference the valve/.test(e.textContent)); });
  ok(r, 'S4 not referenced shown as a prompt');
  r = await banned(); ok(r.length === 0, 'S4 no green or amber at boot: ' + r.join(','));
  // S9: jog row gaps
  r = await ev(() => { bootHomed(); show('valve'); updateUI(); const bs = [...document.querySelectorAll('#dev .b')].filter(b => /Hold to|1 %/.test(b.textContent)).map(b => b.getBoundingClientRect()).sort((a, b) => a.left - b.left);
    return bs.slice(1).map((b, i) => Math.round(b.left - bs[i].right)); });
  ok(r.length === 3 && r.every(g => g >= 12), 'S9 jog row gaps ' + r.join(','));
  // S11: refresh interval
  r = await ev(() => /lastUI > 500/.test(frame.toString()));
  ok(r, 'S11 device values refresh at 2 Hz');
  await h.audit('wp023-final');
};
