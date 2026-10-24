// Bench scenarios against the firmware build (pmc-wasm.js), in node:
//   node simulator/tests/run.js [--png <dir>]
// Each case boots a new board and checks the firmware's own console lines.
const { load } = require('./sim-node.js');
const pngDir = process.argv.includes('--png') ? process.argv[process.argv.indexOf('--png') + 1] : null;
let failed = 0;
function check(name, ok, detail) {
  console.log((ok ? 'ok   ' : 'FAIL ') + name + (ok || !detail ? '' : '\n     ' + detail));
  if (!ok) failed++;
}
function homed(s) {
  s.boot(); s.set(5, 40000); s.run(2000);
  const out = s.cmd('VALVE CAL', 120000);
  if (!/Homed in/.test(out)) throw new Error('homing failed:\n' + out.slice(-800));
  return s;
}
function shot(s, name) { if (pngDir) s.png(require('path').join(pngDir, name + '.png')); }

{ // Boot: NOT HOMED, the supply seen, nothing moves.
  const s = load(); s.boot(); s.set(5, 40000); s.run(3000);
  check('boot NOT HOMED', /Valve: NOT HOMED/.test(s.out), s.out);
  check('boot motor supply seen', /SUP ok/.test(s.out), s.out);
  check('boot no motion', s.get(5) === 40000);
  shot(s, 'boot');
}
{ // Homing from the screen: Home valve, then Home in the question.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.tap(415, 72); s.tap(346, 147); s.run(120000);
  check('home from the screen', /Homed in/.test(s.out) && s.get(30) === 0, s.out.slice(-400));
  shot(s, 'homed');
}
{ // Set position (no homing): VALVE CLOSED takes the count as 0 % where it stands.
  const s = load(); s.boot(); s.run(2000);
  const out = s.cmd('VALVE CLOSED', 1000);
  check('set position by hand', /OK/.test(out) && !/ERR/.test(out), out);
}
{ // MANUAL go-to and the needle follows the coils.
  const s = homed(load());
  s.cmd('VALVE GOTO 25', 60000);
  const full = s.get(5) / 2;
  check('GOTO 25 % moves the needle 25 % of 66048', Math.abs(full - 16512) <= 1, 'needle ' + full);
}
{ // Motor supply off: moves refused, said on the screen and the console.
  const s = homed(load());
  s.set(12, 0); s.run(2000);
  const out = s.cmd('VALVE GOTO 10', 2000);
  check('supply off seen', /SUP OFF/.test(s.out + out) || /supply off/i.test(out), out);
  check('supply off: no move', s.get(5) === 0);
  shot(s, 'supply-off');
}
{ // F-1: Druck unplugged.
  const s = homed(load()); s.set(8, 0); s.run(5000);
  const ev = s.cmd('EVT? 5');
  check('F-1 Druck unplugged', /F-1/.test(ev), ev);
  shot(s, 'f01');
}
{ // F-2: excitation out of 9.5-10.5 V.
  const s = homed(load()); s.set(10, 9.2); s.run(5000);
  const ev = s.cmd('EVT? 5');
  check('F-2 excitation low', /F-2/.test(ev), ev);
}
{ // F-3: the switch never trips during homing.
  const s = load(); s.boot(); s.set(5, 40000); s.set(7, 2); s.run(2000);
  const out = s.cmd('VALVE CAL', 400000);
  check('F-3 limit not found', /F-3/.test(out), out.slice(-600));
}
{ // F-4: homing from the switch, which stays pressed after the clearance move.
  const s = homed(load()); s.set(7, 1);
  const out = s.cmd('VALVE CAL', 60000);
  check('F-4 limit stuck', /F-4/.test(out), out.slice(-600));
}
{ // AUTO at 5 mbar settles.
  const s = homed(load());
  s.cmd('CTRL SP 5'); s.cmd('CTRL AUTO');
  s.out = ''; s.run(400000);
  const last = s.out.split('\n').filter(l => l.startsWith('AUTO')).pop() || '';
  check('AUTO 5 mbar stable', /Stable/.test(last), last);
  shot(s, 'auto');
}
{ // F-6: the needle slips in AUTO, so the pressure does not follow.
  const s = homed(load());
  s.cmd('CTRL SP 5'); s.cmd('CTRL AUTO'); s.run(300000);
  s.set(6, 0); s.cmd('CTRL SP 8');
  s.out = ''; s.run(600000);
  check('F-6 no response', /F-6/.test(s.out), s.out.split('\n').filter(l => /EVT|AUTO/.test(l)).slice(-5).join('\n'));
  shot(s, 'f06');
}
{ // W-2: the rotary pump stops in AUTO; the pressure never comes back into band.
  const s = homed(load());
  s.cmd('CTRL SP 5'); s.cmd('CTRL AUTO'); s.run(300000);
  s.set(4, 0);
  s.out = ''; s.run(1900000);
  check('W-2 to MANUAL', /W-2/.test(s.out), s.out.split('\n').filter(l => /EVT/.test(l)).slice(-5).join('\n'));
  shot(s, 'w02');
}
{ // W-3: controller hot.
  const s = homed(load()); s.set(13, 57); s.run(3000);
  const ev = s.cmd('EVT? 3');
  check('W-3 controller hot', /W-3 Controller hot/.test(ev), ev);
  shot(s, 'w03');
}
{ // Settings and the saved position survive a power cycle (serial flash).
  const s = homed(load());
  s.cmd('VALVE GOTO 10', 20000); s.cmd('PID KP 3'); s.cmd('CFG SAVE', 1000); s.run(5000);
  const flash = s.flash(), needle = s.get(5);
  const t = load(); t.x.sim_bench_init(); t.boot(flash); t.set(5, needle); t.run(3000);
  const pid = t.cmd('PID?'), vp = t.cmd('VALVE SAVED?');
  check('settings kept over a power cycle', /KP 3/.test(pid) || /3\.0+ %\/mbar/.test(pid), pid);
  check('valve position offered after a power cycle', /10\.0|6604|6605/.test(vp), vp);
}
console.log(failed ? failed + ' failed' : 'all passed');
process.exit(failed ? 1 : 0);
