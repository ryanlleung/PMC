// Bench scenarios against the firmware build (pmc-wasm.js), in node:
//   node simulator/tests/run.js [--png <dir>]
// Each case boots a new board and checks the firmware's replies. From 0.13.0
// the board sends nothing unasked, so a case reads the event log (EVNT?).
// The simulator builds with the engineering commands (VALVE CAL, PID, ...);
// remote-only.js checks the operator build without them.
const { load } = require('./sim-node.js');
const pngDir = process.argv.includes('--png') ? process.argv[process.argv.indexOf('--png') + 1] : null;
let failed = 0;
function check(name, ok, detail) {
  console.log((ok ? 'ok   ' : 'FAIL ') + name + (ok || !detail ? '' : '\n     ' + detail));
  if (!ok) failed++;
}
// The last n events, newest last.
const events = (s, n = 100) => s.cmd('EVNT? ' + n, 2000);
// VALVE CAL is an engineering command (the simulator builds with them).
function homed(s) {
  s.boot(); s.set(5, 40000); s.run(2000);
  s.cmd('VALVE CAL', 120000);
  const ev = events(s, 10);
  if (!/Homed in/.test(ev)) throw new Error('homing failed:\n' + ev.slice(-800));
  return s;
}
function shot(s, name) { if (pngDir) s.png(require('path').join(pngDir, name + '.png')); }

{ // Boot: NOT HOMED, nothing moves.
  const s = load(); s.boot(); s.set(5, 40000); s.run(3000);
  check('boot sends nothing unasked', s.out === '', s.out);
  const st = s.cmd('MODE?;VALV?;VALV MOVING?;ALRM?'), ev = events(s);
  check('boot NOT HOMED', st === 'MODE NOT HOMED;VALV NONE;VALV MOVING 0;ALRM NONE\r\n', st);
  check('boot no motion', s.get(5) === 40000);
  shot(s, 'boot');
}
{ // Homing from the screen: Home valve, then Home in the question.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.tap(415, 72); s.tap(346, 147); s.run(120000);
  s.out += events(s, 10);
  // After homing the needle sits 0 or 400 full steps (the final back-off) above the switch. The count is
  // that distance up to 0.12.11 and 0 from 0.12.12 (0 % is the point after the back-off).
  const off = s.get(5) / 2 - s.get(30);
  check('home from the screen', /Homed in/.test(s.out) && [0, 400].includes(s.get(5) / 2) && [0, 400].includes(off), s.out.slice(-400));
  shot(s, 'homed');
}
{ // Set position (no homing): VALVE CLOSED takes the count as 0 % where it stands.
  const s = load(); s.boot(); s.run(2000);
  const out = s.cmd('VALVE CLOSED', 1000);
  check('set position by hand', /OK/.test(out) && !/ERR/.test(out), out);
}
{ // MANUAL go-to and the needle follows the coils.
  const s = homed(load());
  s.cmd('VALV SET 25', 60000);
  const full = s.get(5) / 2;
  // The needle is the count plus the back-off when 0 % is above the switch (0.12.12).
  check('VALV SET 25 % moves the needle 25 % of 66048', Math.abs(s.get(30) - 16512) <= 1 && [0, 400].includes(full - s.get(30)), 'needle ' + full + ', count ' + s.get(30));
}
{ // F-1: Druck unplugged.
  const s = homed(load()); s.set(8, 0); s.run(5000);
  const ev = events(s, 5);
  check('F-1 Druck unplugged', /F-1/.test(ev), ev);
  shot(s, 'f01');
}
{ // F-2: excitation out of 9.5-10.5 V.
  const s = homed(load()); s.set(10, 9.2); s.run(5000);
  const ev = events(s, 5);
  check('F-2 excitation low', /F-2/.test(ev), ev);
}
{ // F-3: the switch never trips during homing.
  const s = load(); s.boot(); s.set(5, 40000); s.set(7, 2); s.run(2000);
  s.cmd('VALVE CAL', 400000);
  const out = events(s, 5);
  check('F-3 limit not found', /F-3/.test(out), out.slice(-600));
}
{ // F-4: homing from the switch, which stays pressed after the clearance move.
  const s = homed(load()); s.set(7, 1);
  s.cmd('VALVE CAL', 60000);
  const out = events(s, 5);
  check('F-4 limit stuck', /F-4/.test(out), out.slice(-600));
}
{ // AUTO at 5 mbar settles.
  const s = homed(load());
  s.cmd('PRES SP 5'); s.cmd('MODE AUTO');
  s.run(400000);
  const v = s.cmd('VALVE?'), d = s.cmd('PRES?;PRES SP?;MODE?;VALV?;ALRM?');
  check('AUTO 5 mbar stable', /target 5\.0 mbar, band [0-9.]+ mbar, stable/.test(v), v);
  check('poll in AUTO', /^PRES [45]\.\d{3} mbar;PRES SP 5\.0 mbar;MODE AUTO;VALV \d+\.\d\d %;ALRM NONE\r\n$/.test(d), d);
  shot(s, 'auto');
}
{ // F-6: the needle slips in AUTO, so the pressure does not follow.
  const s = homed(load());
  s.cmd('PRES SP 5'); s.cmd('MODE AUTO'); s.run(300000);
  s.set(6, 0); s.cmd('PRES SP 8');
  s.run(600000);
  const ev = events(s, 10);
  check('F-6 no response', /F-6/.test(ev), ev);
  shot(s, 'f06');
}
{ // W-2: the rotary pump stops in AUTO; the pressure never comes back into band.
  const s = homed(load());
  s.cmd('PRES SP 5'); s.cmd('MODE AUTO'); s.run(300000);
  s.set(4, 0);
  s.run(1900000);
  const ev = events(s, 10), st = s.cmd('MODE?;ALRM?');
  check('W-2 to MANUAL', /W-2/.test(ev) && /^MODE MANUAL;ALRM W-2,1,/.test(st), ev + st);
  shot(s, 'w02');
}
{ // W-3: controller hot.
  const s = homed(load()); s.set(13, 57); s.run(3000);
  const ev = events(s, 3);
  check('W-3 controller hot', /W-3,Controller hot/.test(ev) && /^ALRM W-3,0,Controller hot\r\n$/.test(s.cmd('ALRM?')), ev);
  shot(s, 'w03');
}
{ // Settings and the saved position survive a power cycle (serial flash).
  const s = homed(load());
  s.cmd('VALV SET 10', 20000); s.cmd('PID KP 3'); s.cmd('CFG SAVE', 1000); s.run(5000);
  const flash = s.flash(), needle = s.get(5);
  const t = load(); t.x.sim_bench_init(); t.boot(flash); t.set(5, needle); t.run(3000);
  const pid = t.cmd('PID?'), vp = t.cmd('VALVE SAVED?');
  check('settings kept over a power cycle', /KP 3/.test(pid) || /3\.0+ %\/mbar/.test(pid), pid);
  check('valve position offered after a power cycle', /10\.0|6604|6605/.test(vp), vp);
}
{ // Limit override is a two-finger deadman (0.13.7): held alone it does nothing; held, a second
  // finger on Close closes past a pressed switch, on Open opens; either finger lifting stops it.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.tap(415, 184); s.tap(379, 184); s.tap(298, 184); s.tap(318, 147);   // Menu > Settings > Motor override > Open
  s.set(7, 1); s.run(500);                                              // switch stuck pressed
  s.tap(186, 184, 1000);
  const c0 = s.get(30), blocked = c0 === 0;                             // Close alone refused
  const over = (d) => s.x.sim_touch(64, 184, d), second = (px, d) => s.x.sim_touch2(px, 184, d);
  over(1); s.run(2000); const alone = s.get(30);
  check('limit override alone does nothing', blocked && alone === c0 && !/Closing|Opening/.test(s.cmd('MOT?')), 'count ' + alone);
  second(415, 1); s.run(1000); second(415, 0); s.run(200);              // second finger on Set zero
  check('second finger elsewhere does nothing', s.get(30) === c0, 'count ' + s.get(30));
  second(186, 1); s.run(3000); const held = s.get(30);
  check('override + Close closes past the switch', held < -500, 'count ' + held);
  shot(s, 'override');
  second(186, 0); s.run(300); const rel = s.get(30); s.run(1000);
  check('lifting the second finger stops at once', s.get(30) === rel && !/override/.test(s.cmd('MOT?')), s.get(30) + ' vs ' + rel);
  second(186, 1); s.run(2000); const again = s.get(30);
  over(0); s.run(300); const rel2 = s.get(30); s.run(1000);
  check('lifting the override finger stops the close', again < rel - 300 && s.get(30) === rel2, again + ' ' + rel2 + ' ' + s.get(30));
  second(186, 0); s.run(300);
  s.x.sim_touch(186, 184, 1); s.run(1000); s.x.sim_touch(186, 184, 0); s.run(500);
  check('Close alone still refused after', s.get(30) === rel2, 'count ' + s.get(30));
  over(1); s.run(200); second(298, 1); s.run(2000); const opened = s.get(30);
  second(298, 0); over(0); s.run(2000);
  check('override + Open opens', opened > rel2 + 300, opened + ' vs ' + rel2);
}
{ // Pressure 21 (0.14.0): CAL AIR without a number takes the room sensor.
  const s = load(); s.boot(); s.run(3000);
  s.set(11, 0); s.run(3000); s.cmd('CAL VAC', 15000);
  s.set(11, 1); s.set(19, 1008.4); s.run(3000);
  const a = s.cmd('CAL AIR', 15000), ev = events(s, 4);
  check('CAL AIR from the room sensor', !/ERR/.test(a) && /Air point [\d.]+ mV\/V at 1008\.40 mbar from the room sensor/.test(ev), a + ev.slice(-400));
  // No room sensor: CAL AIR needs a number, BARO? says NONE.
  const u = load(); u.boot(); u.set(20, 0); u.run(3000); u.set(11, 1); u.run(3000);
  check('BARO NONE without the sensor', u.cmd('BARO?', 1000) === 'BARO NONE\r\n', u.cmd('BARO?', 1000));
  const r = u.cmd('CAL AIR', 2000);
  check('CAL AIR refused without the sensor', /no room pressure sensor: give the pressure/i.test(r), r);
  { const r2 = u.cmd('CAL AIR 1013', 15000); check('CAL AIR 1013 still works', !/ERR/.test(r2), r2); }
}
{ // The remote set (0.13.0): the reply rules of the handover page.
  const s = homed(load());
  const one = c => s.cmd(c, 1000);
  // R1, R2, R5: no echo; a query gives one line starting with its own words.
  const qs = ['PRES?', 'PRES SP?', 'MODE?', 'VALV?', 'VALV MOVING?', 'BARO?', 'ALRM?', 'NETW?', 'TIME?'];
  const bad = qs.map(q => [q, one(q)]).filter(([q, r]) => !(r.startsWith(q.slice(0, -1) + ' ') && /^[^\r\n]*\r\n$/.test(r)))
    .map(([q, r]) => q + ' -> ' + JSON.stringify(r));
  check('queries: one line, own words first', bad.length === 0, bad.join('\n     '));
  // R2: set commands silent when accepted.
  const sets = ['PRES SP 5', 'MODE MANUAL', 'VALV SET 30', 'STOP', 'ALRM OK', 'ALRM RESET', 'TIME SET 2026-10-27 12:00:00'];
  const loud = sets.map(c => [c, s.cmd(c, c.startsWith('VALV') ? 60000 : 1000)]).filter(([, r]) => r !== '').map(([c, r]) => c + ' -> ' + JSON.stringify(r));
  check('set commands silent', loud.length === 0, loud.join('\n     '));
  check('TIME SET then TIME?', /^TIME 2026-10-27 12:00:\d\d\r\n$/.test(one('TIME?')), one('TIME?'));
  // R4, R6, R7: a query confirms the value taken; fixed decimals; chaining.
  check('PRES SP 5.04;PRES SP? gives 5.0', one('PRES SP 5.04;PRES SP?') === 'PRES SP 5.0 mbar\r\n', one('PRES SP?'));
  const poll = one('PRES?;PRES SP?;MODE?;VALV?;ALRM?');
  check('chained poll', /^PRES \d+\.\d{3} mbar;PRES SP 5\.0 mbar;MODE MANUAL;VALV 30\.00 %;ALRM NONE\r\n$/.test(poll), poll);
  check('BARO? and NETW? in the simulator', one('BARO?;NETW?') === 'BARO 1013.20 mbar;NETW NONE\r\n', one('BARO?;NETW?'));
  check('*IDN?', /^MercShore,iPC,[0-9A-F]{24},\d+\.\d+\.\d+\r\n$/.test(one('*IDN?')), one('*IDN?'));
  // EVNT?: the count line, then that many lines, newest first.
  const ev = s.cmd('EVNT? 3', 2000).split('\r\n');
  check('EVNT? 3', ev[0] === 'EVNT 3' && ev.slice(1, 4).every(l => /^\d+\.\d,[A-Z0-9-]+,/.test(l)) && ev.length === 5 && ev[4] === '',
    ev.join(' | '));
  check('EVNT? defaults to 1', s.cmd('EVNT?', 2000).split('\r\n').length === 3);
  // R8: ERR <code> <text>, the first refusal ends the line.
  const errs = [['NO SUCH', 100], ['PRES SP x', 101], ['PRES SP 2000', 102], ['X'.repeat(97), 103], ['MODE?;EVNT? 2', 104],
    ['EVNT? 0', 102], ['VALV SET 101', 102], ['VALV SET', 101], ['MODE', 101], ['TIME SET 2026-13-01 00:00:00', 101],
    ['PRES? 5', 100], ['pres?', 100]];
  const wrong = errs.map(([c, n]) => [c, n, one(c)]).filter(([c, n, r]) => !new RegExp('(^|;)ERR ' + n + ' [A-Z][^;\\r\\n]*\\r\\n$').test(r))
    .map(([c, n, r]) => c.slice(0, 20) + ' -> ' + JSON.stringify(r) + ' (want ' + n + ')');
  check('error codes 100-104', wrong.length === 0, wrong.join('\n     '));
  check('first refusal ends the line', one('PRES SP 2000;PRES SP?') === 'ERR 102 Outside 0 to 1034.2 mbar\r\n', one('PRES SP 2000;PRES SP?'));
  // The page's example: AUTO, then VALV SET refused until MANUAL.
  check('MODE AUTO;MODE?', one('MODE AUTO;MODE?') === 'MODE AUTO\r\n', one('MODE?'));
  check('ERR 202 in AUTO', one('VALV SET 40;VALV MOVING?') === 'ERR 202 Switch to MANUAL first\r\n');
  check('MODE MANUAL;VALV SET 40;VALV MOVING?', one('MODE MANUAL;VALV SET 40;VALV MOVING?') === 'VALV MOVING 1\r\n');
  s.run(30000);
  check('STOP in AUTO ends AUTO', (one('MODE AUTO'), one('STOP;MODE?')) === 'MODE MANUAL\r\n');
  // 201 while homing, 204 not homed, 206 no target, 210 cause present.
  s.cmd('VALVE CAL', 200);
  check('MODE HOMING, moving', one('MODE?;VALV MOVING?') === 'MODE HOMING;VALV MOVING 1\r\n', one('MODE?'));
  check('ERR 201 while homing', one('VALV SET 10') === 'ERR 201 Homing, STOP to cancel\r\n', one('VALV SET 10'));
  one('STOP');
  const u = load(); u.boot(); u.run(2000);
  check('ERR 204 not homed', u.cmd('PRES SP 5;MODE AUTO;MODE?') === 'ERR 204 Home the valve first\r\n', u.cmd('MODE?'));
  check('VALV SET not homed', u.cmd('VALV SET 40') === 'ERR 204 Home the valve first\r\n', u.cmd('VALV SET 40'));
  const t = homed(load());
  check('ERR 206 no target', t.cmd('MODE AUTO') === 'ERR 206 Set a target first\r\n');
  t.set(8, 0); t.run(5000);
  check('ERR 210 cause present', /^ERR 210 Not reset, cause still present: F-1\r\n$/.test(t.cmd('ALRM RESET')), t.cmd('ALRM RESET'));
  check('ALRM? fault flag 0 while its cause is present', /^ALRM F-1,0,/.test(t.cmd('ALRM?')), t.cmd('ALRM?'));
  t.set(8, 1); t.run(5000);
  check('ALRM? fault flag 1 once it can reset', /^ALRM F-1,1,/.test(t.cmd('ALRM?')), t.cmd('ALRM?'));
  check('ALRM RESET clears it', t.cmd('ALRM RESET;ALRM?') === 'ALRM NONE\r\n', t.cmd('ALRM?'));
  check('nothing sent unasked over a run', (t.cmd('PRES SP 5;MODE AUTO', 60000), t.out === ''), t.out);
}
{ // Remote control switch (0.13.4): On by default (0.13.7); Off, the link takes queries only; On from the screen, it takes sets.
  const d = load(); d.boot(); d.run(1000);   // boot() leaves the switch as the firmware starts it
  check('switch On by default', d.cmd('PRES SP 5;PRES SP?') === 'PRES SP 5.0 mbar\r\n', d.cmd('PRES SP?'));
  const s = load(); s.boot(undefined, { remote: false }); s.set(5, 40000); s.run(2000);
  check('switch Off: PRES SP refused', s.cmd('PRES SP 5') === 'ERR 211 Read only over the network\r\n', s.cmd('PRES SP 5'));
  check('switch Off: STOP refused', s.cmd('STOP') === 'ERR 211 Read only over the network\r\n');
  check('switch Off: queries answer', s.cmd('MODE?;PRES SP?') === 'MODE NOT HOMED;PRES SP NONE\r\n', s.cmd('MODE?;PRES SP?'));
  s.tap(415, 184); s.tap(379, 184); s.tap(120, 72); s.tap(357, 184);   // Menu, Settings, System, Remote control
  check('switch On from the screen: PRES SP taken', s.cmd('PRES SP 5;PRES SP?') === 'PRES SP 5.0 mbar\r\n', s.cmd('PRES SP?'));
  check('switch On is a NET event', /NET,Remote control on \(screen\)/.test(events(s, 5)), events(s, 5));
  shot(s, 'system');
  s.tap(357, 184);
  check('switch Off again: refused', s.cmd('PRES SP 6') === 'ERR 211 Read only over the network\r\n');
}
{ // Homing return (0.13.6): its own setting, the step 4 move off the switch; 0 % is there.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  check('VALVE RETURN 150', /^OK VALVE RETURN 150 steps/.test(s.cmd('VALVE RETURN 150')), s.cmd('VALVE?'));
  s.cmd('VALVE CAL', 120000);
  const ev = events(s, 3);
  check('homed with the switch 150 steps below 0 %', /Homed in \d+ s, switch at -150, 0 % at 0/.test(ev) && s.get(5) === 300, ev + ' needle ' + s.get(5));
}
console.log(failed ? failed + ' failed' : 'all passed');
process.exit(failed ? 1 : 0);
