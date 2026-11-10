// Replays a bench session's logged valve moves in the simulator's bench
// cryostat, to put the two side by side (plot_bench.py draws them):
//   node bench-replay.js session.csv [from_s] [to_s] > replay.csv
// session.csv: a control test GUI session log (t_s, p_mbar, valve_pct,
// stage, A_heat_exchanger_K, ...). The simulator first holds the log's
// first pressure in AUTO for 30 min (KP 0.40 TI 0.75), so it starts where
// the bench did, heat exchanger included; then it goes to MANUAL and its
// valve follows the logged valve, second by second, plus a fixed offset:
// its own valve for that first pressure less the bench's (printed on
// stderr). Open loop: the same valve moves, and what the pressure and the
// heat exchanger do in each.
const fs = require('fs'), path = require('path');
const { load } = require(path.join(__dirname, '..', 'tests', 'sim-node.js'));
const [file, a, b] = process.argv.slice(2);
if (!file) { console.error('node bench-replay.js session.csv [from_s] [to_s] > replay.csv'); process.exit(2); }
// One CSV line into fields ("quoted, with commas" and "" inside quotes).
function fields(line) {
  const out = []; let f = '', q = false;
  for (let i = 0; i < line.length; i++) {
    const c = line[i];
    if (q) { if (c === '"' && line[i + 1] === '"') { f += '"'; i++; } else if (c === '"') q = false; else f += c; }
    else if (c === '"') q = true; else if (c === ',') { out.push(f); f = ''; } else f += c;
  }
  out.push(f); return out;
}
const quote = v => /[",\r\n]/.test(v) ? '"' + v.replace(/"/g, '""') + '"' : v;
const lines = fs.readFileSync(file, 'utf8').trim().split(/\r?\n/);
const hdr = fields(lines[0]), ix = n => hdr.indexOf(n);
const t0 = a === undefined ? -Infinity : Number(a), t1 = b === undefined ? Infinity : Number(b);
const rows = lines.slice(1).map(fields)
  .filter(r => r[ix('p_mbar')] !== '' && r[ix('valve_pct')] !== '' && +r[ix('t_s')] >= t0 && +r[ix('t_s')] <= t1)
  .map(r => ({ t: +r[ix('t_s')], p: +r[ix('p_mbar')], x: +r[ix('valve_pct')],
               hx: ix('A_heat_exchanger_K') < 0 ? '' : r[ix('A_heat_exchanger_K')], st: r[ix('stage')] || '' }));
if (!rows.length) { console.error('no samples in that range'); process.exit(1); }
const num = q => parseFloat((q.match(/-?[\d.]+/) || ['NaN'])[0]);
const s = load(); s.boot(undefined, { plant: 0 }); s.set(5, 40000); s.run(2000); s.home();
s.cmd('PID SET 0.40,0.75,0'); s.cmd('PRES SP ' + rows[0].p.toFixed(1)); s.cmd('MODE AUTO'); s.run(1800000);
const off = +(num(s.cmd('VALV?')) - rows[0].x).toFixed(2);
s.cmd('MODE MANUAL');
console.error('valve offset %s %% (sim %s mbar, heat exchanger %s K at the start)', off, s.get(0).toFixed(2), s.get(23).toFixed(2));
console.log('t_s,bench_p,sim_p,bench_valve,sim_valve,bench_hx,sim_hx,stage,offset');
let sent = null, tp = rows[0].t;
for (const r of rows) {
  const dt = Math.round((r.t - tp) * 1000); tp = r.t;
  if (dt > 0) s.run(dt);
  const want = Math.max(0, +(r.x + off).toFixed(2));
  if (sent === null || Math.abs(want - sent) >= 0.01) { sent = want; s.send('VALV SET ' + sent.toFixed(2)); }
  console.log([r.t.toFixed(1), r.p, s.get(0).toFixed(3), r.x, sent, r.hx, s.get(23).toFixed(2), quote(r.st), off].join(','));
}
