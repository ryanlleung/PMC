// Label jitter check: a label that keeps its text must keep its place as
// a reading rolls over (a digit, the zone in use). Every
// screen reached in up to 3 taps from Home, with the pressure held near a
// digit or zone edge in MANUAL and at setpoints on the edges in AUTO,
// sampled every 0.1 s for 6 s. Lists:
//  - a label whose text stays the same but whose box moves;
//  - a time of day (hh:mm:ss) that goes back.
// Labels only: the Events rows and the Trend are drawn, not labels (the
// Events times have their own checks in run.js).
//   node simulator/tests/label-jitter.js [file.wasm]
// Exits 1 if anything moves.
const { load } = require('./sim-node.js');
const wasm = process.argv[2];
const cstr = (s, p) => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
const snap = s => new Uint8Array(s.x.memory.buffer).slice();
const restore = (s, m) => new Uint8Array(s.x.memory.buffer).set(m);
function labels(s) {
  const out = [];
  for (const line of cstr(s, s.x.sim_textfit()).split('\n')) {
    const m = /^.*? \| "(.*)" \| text \d+x\d+, box (\d+)x(\d+) at (-?\d+),(-?\d+)/.exec(line);
    if (m) out.push({ t: m[1], box: m[4] + ',' + m[5] + ' ' + m[2] + 'x' + m[3] });
  }
  return out;
}
function manual(pct = 30) {
  const s = load(wasm); s.boot(); s.run(2000); s.setClosed(); s.cmd('VALVE SET ' + pct, 1000);
  for (let t = 0; t < 600000 && s.busy(); t += 1000) s.run(1000);
  s.run(5000); return s;
}
let rnd = 1;   // the same noise every run
const noise = () => ((rnd = (rnd * 1103515245 + 12345) % 2147483648) / 2147483648 - 0.5);
const found = new Map();
let drive = null, samples = 0;
function sample(s, where) {
  const hist = [];
  for (let i = 0; i < 60; i++) {
    if (drive !== null) s.set(40, drive + noise() * 0.24);
    s.run(100); hist.push(labels(s)); samples++;
  }
  // Same text, same number of labels with it, other boxes.
  const seen = new Map();
  for (const ls of hist) {
    const g = new Map();
    for (const l of ls) { if (!g.has(l.t)) g.set(l.t, []); g.get(l.t).push(l.box); }
    for (const [t, b] of g) {
      const k = t + '|' + b.length, v = b.sort().join(' ; ');
      if (!seen.has(k)) seen.set(k, new Set());
      seen.get(k).add(v);
    }
  }
  for (const [k, set] of seen) if (set.size > 1) {
    const t = k.split('|')[0];
    if (!found.has('m|' + t)) found.set('m|' + t, where + ' | "' + t + '" moves: ' + [...set].join('  /  '));
  }
  // A time of day going back, at one box.
  const byBox = new Map();
  for (const ls of hist) for (const l of ls) { if (!byBox.has(l.box)) byBox.set(l.box, []); byBox.get(l.box).push(l.t); }
  for (const [box, ts] of byBox) {
    const tm = ts.map(t => /(\d\d):(\d\d):(\d\d)/.exec(t)).map(m => m ? +m[1] * 3600 + +m[2] * 60 + +m[3] : null);
    for (let i = 1; i < tm.length; i++) if (tm[i] !== null && tm[i - 1] !== null && tm[i] < tm[i - 1] && !found.has('b|' + box))
      found.set('b|' + box, where + ' | time went back at ' + box + ': ' + ts[i - 1] + ' then ' + ts[i]);
  }
}
function crawl(s, root, maxDepth = 3) {
  const seen = new Set(), start = snap(s);
  let queue = [[]];
  for (let depth = 0; depth <= maxDepth && queue.length; depth++) {
    const next = [];
    for (const p of queue) {
      restore(s, start);
      for (const [x, y] of p) s.tap(x, y, 600);
      const title = cstr(s, s.x.sim_screen()), pop = cstr(s, s.x.sim_popup_text()), clicks = cstr(s, s.x.sim_clickables());
      const sig = title + '|' + pop + '|' + clicks;
      if (seen.has(sig) || pop) continue;   // popups hold still
      seen.add(sig);
      sample(s, root + ' > ' + title + (p.length ? ' (' + p.map(q => q[2] + '@' + q[0] + ',' + q[1]).join(' ') + ')' : ''));
      if (depth < maxDepth)
        for (const c of clicks.split(';').filter(Boolean)) { const [x, y] = c.split(',').map(Number); next.push(p.concat([[x, y, title]])); }
    }
    queue = next;
  }
  restore(s, start);
  return seen.size;
}
const t0 = Date.now();
let screens = 0;
// MANUAL near the zone edges (6, 10, 14 mbar), the digit edges (1, 10, 100);
// AUTO at setpoints on the zone edges.
for (const a of ['m0.95', 'm6', 'm10', 'm14', 'm99.95', '1', '6', '10', '14']) {
  const s = manual(30); s.x.sim_textfit_all(1);
  const v = parseFloat(a.replace('m', ''));
  if (a[0] === 'm') { drive = v; for (let i = 0; i < 100; i++) { s.set(40, v); s.run(100); } }
  else { drive = null; s.cmd('SETPOINT ' + v + ';MODE AUTO', 1000); s.run(90000); }
  screens += crawl(s, (drive === null ? 'AUTO at ' : 'MANUAL near ') + v + ' mbar');
}
for (const v of found.values()) console.log(v);
console.log(found.size + ' problems on ' + screens + ' screens, ' + samples + ' samples, ' + Math.round((Date.now() - t0) / 1000) + ' s');
process.exit(found.size ? 1 : 0);
