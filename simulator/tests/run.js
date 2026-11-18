// Bench scenarios against the firmware build (ipc-wasm.js), in node:
//   node simulator/tests/run.js [--png <dir>]
// Each case boots a new board and checks the firmware's replies. The board
// sends nothing unasked, so a case reads the event log (EVENT?). The text
// port takes the remote command set only; homing, calibration and settings
// go through the simulator's test hooks (sim-node.js), as the screens do.
const { load, CFG } = require('./sim-node.js');
const fs = require('fs'), path = require('path');
const firmwareVersion = fs.readFileSync(path.join(__dirname, '../../firmware/ipc_config.h'), 'utf8')
  .match(/#define\s+IPC_FW_VERSION\s+"([^"]+)"/)[1];
const pngDir = process.argv.includes('--png') ? process.argv[process.argv.indexOf('--png') + 1] : null;
let failed = 0;
function check(name, ok, detail) {
  console.log((ok ? 'ok   ' : 'FAIL ') + name + (ok || !detail ? '' : '\n     ' + detail));
  if (!ok) failed++;
}
// The last n events, newest last.
const events = (s, n = 100) => s.cmd('EVENT? ' + n, 2000);
// The simulated flow line gives far less pressure per % open than the bench cryostat (0.8 to 12.5 mbar over
// 20 to 100 % open), so tests of other things in AUTO on it run one PID, KP 4.00 TI 0.25. The factory table
// is the bench cryostat's (6/10/14 mbar, 0.06/0.60, 0.12/0.82, 0.80/0.94, 0.40/0.25). The PID table is always
// on: one PID means every zone the same.
function allSets(s, kp, ti, kd = 0) {
  for (let n = 1; n <= 4; n++) s.cmd('PIDS SET ' + n + ',' + kp + ',' + ti + ',' + kd, 1000);
  return s;
}
function oldGains(s) { return allSets(s, 4, 0.25); }
function homed(s) {
  s.boot(); s.set(5, 40000); s.run(2000);
  s.home();
  const ev = events(s, 10);
  if (!/Homed in/.test(ev)) throw new Error('homing failed:\n' + ev.slice(-800));
  return s;
}
function shot(s, name) { if (pngDir) s.png(path.join(pngDir, name + '.png')); }

{ // A stale committed bundle must not silently test an older firmware release.
  const s = load();
  check('bundle matches IPC_FW_VERSION', s.version() === firmwareVersion, s.version() + ' vs ' + firmwareVersion);
}

{ // Boot: NOT HOMED, nothing moves.
  const s = load(); s.boot(); s.set(5, 40000); s.run(3000);
  check('boot sends nothing unasked', s.out === '', s.out);
  const st = s.cmd('MODE?;VALVE?;VALVE MOVING?;ALARM?'), ev = events(s);
  check('boot NOT HOMED', st === 'MODE NOT HOMED;VALVE NONE;VALVE MOVING 0;ALARM NONE\r\n', st);
  check('boot no motion', s.get(5) === 40000);
  shot(s, 'boot');
  // Home: Start AUTO is greyed, and Home valve (in the Valve tile) only asks, before a reference is set.
  s.tap(93, 184); s.run(1000); s.tap(269, 184); s.run(1000);
  check('Home moves blocked before homing', s.get(5) === 40000 && s.get(30) === 0 && !s.busy() &&
    s.cmd('MODE?') === 'MODE NOT HOMED\r\n', s.cmd('MODE?'));
}
{ // Homing from the screen: Home valve (the Valve tile), then Home in the question.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.tap(269, 184); s.tap(346, 147); s.run(120000);
  s.out += events(s, 10);
  // The current firmware defines count 0 at the independent Return point above the switch.
  const off = s.get(5) / 2 - s.get(30);
  check('home from the screen', /Homed in/.test(s.out) && s.get(30) === 0 && off === s.setting(CFG.RETURN), s.out.slice(-400));
  shot(s, 'homed');
}
{ // Start AUTO in MANUAL: AUTO at the pressure as shown, from where the valve is, no question.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000); s.run(400000);
  const p = s.cmd('PRESSURE?').match(/PRESSURE ([\d.]+) mbar/)[1], before = s.get(30);
  // The tiles and header follow within 0.15 s of the release, not at the 0.5 s refresh.
  const hdrPx = () => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[3 * 480 + 415];
  const press = (x, y) => { s.x.sim_touch(x, y, 1); s.run(120); s.x.sim_touch(x, y, 0); s.run(150); };
  const manualPx = hdrPx();
  press(93, 184);
  check('Start AUTO shows within 0.15 s', hdrPx() !== manualPx);
  s.run(850);
  const sp = s.cmd('SETPOINT?'), shown = (Math.round(parseFloat(p) * 10) / 10).toFixed(1);
  check('Start AUTO starts AUTO at the shown pressure', s.cmd('MODE?') === 'MODE AUTO\r\n' &&
    sp === 'SETPOINT ' + shown + ' mbar\r\n', p + ' ' + sp);
  // Bumpless: only the loop's own small correction, under 0.5 % of travel.
  check('Start AUTO does not jump the valve', Math.abs(s.get(30) - before) < 330, before + ' -> ' + s.get(30));
  shot(s, 'start-auto');
  s.run(300000);
  check('Start AUTO holds the pressure', s.reg() >= 2, 'regulation ' + s.reg());
  // Stop AUTO (the Valve tile, AUTO): MANUAL, the valve stays where it is, no question.
  const at = s.get(30), autoPx = hdrPx();
  press(269, 184);
  check('Stop AUTO shows within 0.15 s', hdrPx() !== autoPx && hdrPx() === manualPx);
  s.run(2000);
  check('Stop AUTO goes to MANUAL', s.cmd('MODE?') === 'MODE MANUAL\r\n', s.cmd('MODE?'));
  check('Stop AUTO leaves the valve', Math.abs(s.get(30) - at) < 330, at + ' -> ' + s.get(30));
}
{ // The IP tile on Home shows the address/prefix
  // and opens System, as Menu > System does.
  const hdr = (s) => Buffer.from(new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 40).slice().buffer).toString('base64');
  const a = homed(load()), home = hdr(a);
  shot(a, 'home-ip');
  a.tap(415, 128); shot(a, 'home-ip-system');
  const b = homed(load());
  b.tap(415, 184); b.tap(84, 184);   // Menu > System (bottom left)
  check('Home IP tile opens System', hdr(a) === hdr(b) && hdr(a) !== home);
}
{ // The Menu: an icon on every tile, no row captions; nine tiles in three rows of three, each opening its page
  // in one tap; the ninth, Reset to factory, asks.
  const s = homed(load());
  const screen = () => { const m = new Uint8Array(s.x.memory.buffer); let p = s.x.sim_screen(), e = p; while (m[e]) e++;
    return Buffer.from(m.subarray(p, e)).toString(); };
  // Home (its slot is the power icon on Home itself), then Menu: on the Menu that spot is its last tile.
  const menu = () => { if (screen() !== 'Home') s.tap(22, 20); s.tap(415, 184); };
  menu();
  check('Menu opens', screen() === 'Menu', screen());
  shot(s, 'menu');
  // Trend, Events, PID / Motor, Valve, Pressure sensor / System, Factory reset, Diagnostics.
  const tiles = [[84, 72, 'Trend'], [240, 72, 'Events'], [396, 72, 'PID'], [84, 128, 'Motor'],
    [240, 128, 'Valve'], [396, 128, 'Pressure sensor'], [84, 184, 'System'], [396, 184, 'Diagnostics']];
  const wrong = tiles.map(([x, y, t]) => { menu(); s.tap(x, y); return [t, screen()]; })
    .filter(([t, got]) => t !== got).map(([t, got]) => t + ' -> ' + got);
  check('every Menu tile opens its page', wrong.length === 0, wrong.join(', '));
  menu(); s.tap(396, 184); const diag = screen(); s.tap(66, 20);
  check('Back from a page returns to the Menu', diag === 'Diagnostics' && screen() === 'Menu', diag + ' then ' + screen());
  // No dark blue tile: every tile a white card.
  const px = (x, y) => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[y * 480 + x];
  check('every Menu tile white', px(330, 168) === 0xFFFF && px(18, 168) === 0xFFFF && px(174, 168) === 0xFFFF,
    [px(330, 168), px(18, 168), px(174, 168)].join());
  s.tap(240, 128); shot(s, 'valve'); s.tap(66, 20);
  s.tap(84, 128); shot(s, 'motor'); s.tap(66, 20);
  // One sensor per board: the setting keys end at 32 (no sensor choice) and the sensor range is the 15 psia
  // Druck's.
  check('no sensor-in-use setting', s.setting(32) === -2147483648 && s.setting(CFG.DB) === 8 && s.setting(CFG.B3) === 140 &&
    s.setting(CFG.PASS_LO) === 5 && s.setting(CFG.PASS_HI) === 50 && s.setting(CFG.SPT) === 16512 && s.setting(CFG.JOG) === 100,
    [32, CFG.DB, CFG.B3, CFG.PASS_LO, CFG.PASS_HI, CFG.SPT, CFG.JOG].map(k => s.setting(k)).join(' '));
  check('setpoint limit is the 15 psia range', s.cmd('SETPOINT 1035') === 'ERR 102 Outside 0 to 1034.2 mbar\r\n', s.cmd('SETPOINT 1035'));
  // Motor: Jog | Settings, Jog first, no question; Steps/turn on Settings.
  menu(); s.tap(84, 128); const opened = screen(), jogPopup = s.popup(); shot(s, 'motor-jog');
  s.tap(178, 72); shot(s, 'motor-settings');   // Settings (two tabs in two columns)
  s.tap(181, 184); s.tap(40, 239); s.tap(110, 183); s.tap(110, 183); s.tap(110, 183); s.tap(110, 183); s.tap(320, 212);   // Steps/turn 15555
  const spt = s.setting(CFG.SPT); s.tap(66, 20);
  check('Motor: Jog first, no question; Steps/turn from its card, in use at once; Back to the Menu',
    opened === 'Motor' && !jogPopup && spt === 15555 && screen() === 'Menu', [opened, jogPopup, spt, screen()].join());
}
{ // Motor's valve %: Jog | Settings in two columns, then a Valve, % card, then Help; "----" until homed, then the
  // opening, following a move, on both tabs.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const dump = () => { s.x.sim_textfit_all(1); const d = str(s.x.sim_textfit()); s.x.sim_textfit_all(0); return d; };
  const pct = () => (/\| "([^"]*)" \| text \d+x\d+, box \d+x\d+ at 256,72/.exec(dump()) || [])[1];
  const at = t => (new RegExp('\\| "' + t + '" \\| text \\d+x\\d+, box \\d+x\\d+ at (\\d+),').exec(dump()) || [])[1];
  s.tap(415, 184); s.tap(84, 128);
  const unhomed = pct(), cap = at('Valve, %'), jogX = +at('Jog'), setX = +at('Settings'), helpX = +at('Help');
  s.home(); s.cmd('VALVE SET 20', 60000); s.run(500);
  const p20 = pct(); s.tap(178, 72); s.cmd('VALVE SET 30', 60000); s.run(500);
  const p30 = pct();
  check('Motor: Jog and Settings in two columns, Valve, % in the third ("----" until homed, then following a move), Help in the fourth',
    unhomed === '----' && +cap === 256 && jogX < 123 && setX >= 123 && setX < 236 && helpX > 361 && p20 === '20.00' && p30 === '30.00',
    [unhomed, cap, jogX, setX, helpX, p20, p30].join());
}
{ // PID tile opens PID tuning; KD is a saved setting from its keypad.
  const s = homed(load());
  shot(s, 'home-pid');
  s.tap(415, 72); shot(s, 'pid-tuning');
  s.tap(298, 184); s.tap(40, 239); s.tap(250, 239); s.tap(110, 183); s.tap(320, 212);   // zone 1's KD (the zone row), 1.5, Set
  s.tap(415, 128);   // Save: at once
  check('KD saves from the PID tuning page', s.setting(CFG.KD) === 150, 'KD ' + s.setting(CFG.KD));
  // PID table: the Gains tab shows the zone row, four cards of 82 px at x 10, 97, 184 and 271; Zone 3's KP from
  // its card.
  s.cmd('TABLE SET 6,10,14,15', 1000); s.run(1000);
  shot(s, 'pid-tuning-sets');
  s.tap(221, 128); s.tap(62, 184); s.tap(40, 239); s.tap(250, 239); s.tap(110, 183); s.tap(320, 212);   // Zone 3, KP 1.5, Set
  s.tap(415, 128);
  check('Zone 3 KP saves from the zone row', s.setting(CFG.KP3) === 150 && s.setting(CFG.KP) === 6 && s.setting(CFG.KP2) === 12 &&
    s.setting(CFG.KP4) === 40, [s.setting(CFG.KP), s.setting(CFG.KP2), s.setting(CFG.KP3), s.setting(CFG.KP4)].join());
  shot(s, 'pid-tuning-set3');
  s.tap(304, 128); s.tap(62, 184); s.tap(40, 239); s.tap(250, 239); s.tap(320, 212);   // Zone 4, KP 1., Set
  s.tap(415, 128);
  check('Zone 4 KP saves from the zone row', s.setting(CFG.KP4) === 100 && s.setting(CFG.KP3) === 150,
    [s.setting(CFG.KP3), s.setting(CFG.KP4)].join());
  shot(s, 'pid-tuning-set4');
  // Zones: B1, B2 and B3 in a row, the hysteresis under B1. B1 from its card.
  s.tap(182, 72); s.run(500);
  shot(s, 'pid-tuning-table');
  s.tap(65, 184); s.tap(110, 183); s.tap(320, 212);   // 1 to 2 (B1), 5, Set
  s.tap(415, 128);
  check('Set ranges: B1 saves from the first card', s.setting(CFG.B1) === 50 && s.setting(CFG.B2) === 100, s.setting(CFG.B1));
  // Settings: AUTO min and the setpoint band beside it.
  s.tap(291, 72); s.run(500);
  shot(s, 'pid-tuning-auto');
  s.tap(270, 128); s.tap(40, 239); s.tap(320, 212);   // SP band, 1, Set
  s.tap(415, 128);
  check('AUTO: the setpoint band saves from the second card', s.setting(CFG.BAND) === 100, s.setting(CFG.BAND));
}
{ // A tap on anything greyed round a popup closes it, as Back: the dimmed page, the greyed strip, the greyed header
  // icons. A tap on the box does not.
  const s = homed(load());
  // The header's title, left of the remote light: Home's, a keypad's or another page's.
  const title = () => { const fb = new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272); let h = 0;
    for (let y = 4; y < 36; y++) for (let x = 96; x < 290; x++) h = (h * 31 + fb[y * 480 + x]) >>> 0; return h; };
  const home = title(), at = s.cmd('VALVE?');
  s.tap(290, 184); const kp = s.popup();                           // Go to: the keypad
  s.tap(110, 183); s.tap(100, 70); const kpStays = s.popup();       // 5, then a tap on its title
  shot(s, 'popup-keypad');
  s.tap(415, 72); s.run(2000);                                      // the dimmed page right of it (the PID tile under it)
  check('keypad: stays on a tap on its box, closes on the dimmed page; nothing set, still Home', kp && kpStays && !s.popup() &&
    s.cmd('VALVE?') === at && !s.busy() && title() === home, [kp, kpStays, s.popup(), s.cmd('VALVE?'), title() === home].join());
  s.tap(24, 20); const sa = s.popup();                              // the power icon: Shut down?
  s.tap(240, 62); const sb = s.popup();                             // a tap on its question
  shot(s, 'popup-shutdown');
  // A question ignores taps round it for its first second; its box is 438 wide.
  s.tap(470, 100); const sc = !s.popup();                           // the dimmed page right of the box
  s.tap(24, 20); s.tap(100, 240); const guard = s.popup(); s.run(600); s.tap(100, 240); const sd = guard && !s.popup();   // the greyed strip
  s.tap(24, 20); s.run(1000); s.tap(24, 20); const se = !s.popup(); // the greyed power icon
  check('Shut down question: closes on the dimmed page, the greyed strip and the greyed power icon, not on its box; nothing done',
    sa && sb && sc && sd && se && s.cmd('MODE?') === 'MODE MANUAL\r\n', [sa, sb, sc, sd, se].join() + ' ' + s.cmd('MODE?'));
  s.tap(415, 72); s.tap(182, 72); s.tap(65, 184); const pa = s.popup();   // PID > Zones > 1 to 2: its keypad
  s.tap(100, 54); const pb = s.popup();                             // its caption
  shot(s, 'popup-card-keypad');
  s.tap(415, 200);                                                  // the dimmed page (Discard changes under it)
  check('keypad of a settings card: stays on a tap on its caption, closes on the dimmed page; nothing set',
    pa && pb && !s.popup() && s.setting(CFG.B1) === 60, [pa, pb, s.popup(), s.setting(CFG.B1)].join());
  s.tap(415, 72); const ha = s.popup();                             // Help
  s.tap(100, 240); const hb = !s.popup();                           // the greyed strip under it
  check('Help: closes on a tap on the greyed strip', ha && hb, [ha, hb].join());
}
{ // A change made over the network shows on the strip ("Network: ..."), a NET event; reading (PRESSURE?) shows
  // nothing.
  const s = homed(load()); s.run(12000);
  const strip = () => { const fb = new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272); let h = 0;
    for (let y = 222; y < 266; y++) for (let x = 12; x < 250; x++) h = (h * 31 + fb[y * 480 + x]) >>> 0; return h; };
  const h0 = strip();
  for (let i = 0; i < 5; i++) { s.send('PRESSURE?'); s.run(1000); }
  const quiet = strip() === h0;
  const r = s.cmd('SETPOINT 7.5', 600);
  shot(s, 'network-change-strip');
  const shown = strip() !== h0, ev = events(s, 3);
  s.run(6000); const gone = strip() === h0;
  check('network change: SETPOINT shows on the strip and logs NET; polling shows nothing; the message clears',
    r === '' && quiet && shown && /NET.*Text port: setpoint 7\.5 mbar/.test(ev) && gone,
    [r.trim(), quiet, shown, gone].join() + ' ' + ev);
}
{ // Set position (no homing): Set as 0 % takes the count as 0 where it stands.
  const s = load(); s.boot(); s.run(2000);
  const ok = s.setClosed(), st = s.cmd('MODE?;VALVE?');
  check('set position by hand reads 0.00 %', ok && st === 'MODE MANUAL;VALVE 0.00 %\r\n', s.why() + ' ' + st);
}
{ // Set as 0 % after homing reads 0.00 %, not the seat.
  const s = homed(load());
  s.cmd('VALVE SET 10', 30000); s.cmd('STOP');
  const ok = s.setClosed(), st = s.cmd('VALVE?');
  check('Set as 0 % after homing reads 0.00 %', ok && st === 'VALVE 0.00 %\r\n', s.why() + ' ' + st);
}
{ // MANUAL go-to and the needle follows the coils.
  const s = homed(load());
  s.cmd('VALVE SET 25', 60000);
  const full = s.get(5) / 2;
  // The needle is the count plus Return, since 0 % is above the switch.
  check('VALVE SET 25 % moves the needle 25 % of 66048', Math.abs(s.get(30) - 16512) <= 1 && full - s.get(30) === s.setting(CFG.RETURN), 'needle ' + full + ', count ' + s.get(30));
}
{ // F-1: Druck unplugged.
  const s = homed(load()); s.set(8, 0); s.run(5000);
  const ev = events(s, 5);
  check('F-1 Druck unplugged', /F-1/.test(ev), ev);
  shot(s, 'f01');
}
{ // The excitation is a cause of F-1: the reading is a ratio, so 9.2 V raises nothing and reads the same pressure;
  // under 7 V there is no reading, which is F-1.
  const s = homed(load()); s.run(3000);
  const p0 = s.cmd('PRESSURE?');
  s.set(10, 9.2); s.run(5000);
  const p1 = s.cmd('PRESSURE?'), a1 = s.cmd('ALARM?');
  const mb = t => parseFloat(t.replace(/^PRESSURE /, ''));
  check('excitation 9.2 V: no fault, the pressure reads the same', a1 === 'ALARM NONE\r\n' && Math.abs(mb(p1) - mb(p0)) <= 0.05,
    [p0.trim(), p1.trim(), a1.trim()].join(' | '));
  s.set(10, 6.5); s.run(5000);
  const ev = events(s, 5), a2 = s.cmd('ALARM?');
  check('excitation 6.5 V: F-1 names it, no F-2', /F-1,No pressure signal \(sensor supply 6\d\d\d mV, under 7000 mV\)/.test(ev) &&
    /^ALARM F-1,0,No pressure signal\r\n$/.test(a2), ev + a2);
  s.set(10, 10); s.run(3000);
  check('excitation back: F-1 can be reset', /^ALARM F-1,1,/.test(s.cmd('ALARM?')) && s.cmd('ALARM RESET;ALARM?') === 'ALARM NONE\r\n', s.cmd('ALARM?'));
}
{ // F-2: the switch never trips during homing.
  const s = load(); s.boot(); s.set(5, 40000); s.set(7, 2); s.run(2000);
  s.home(400000);
  const out = events(s, 5);
  check('F-2 limit not found', /F-2,Homing: limit switch not found/.test(out) && /^ALARM F-2,1,Limit switch not found\r\n$/.test(s.cmd('ALARM?')),
    out.slice(-600) + s.cmd('ALARM?'));
}
{ // F-3: homing from the switch, which stays pressed after the clearance move.
  const s = homed(load()); s.set(7, 1);
  s.home(60000);
  const out = events(s, 5);
  check('F-3 limit stuck', /F-3,Limit switch still pressed/.test(out) && /^ALARM F-3,1,Limit switch stays pressed\r\n$/.test(s.cmd('ALARM?')),
    out.slice(-600) + s.cmd('ALARM?'));
  shot(s, 'f03');
}
{ // AUTO at 5 mbar settles.
  const s = oldGains(homed(load()));
  s.cmd('SETPOINT 5'); s.cmd('MODE AUTO');
  s.run(400000);
  const d = s.cmd('PRESSURE?;SETPOINT?;MODE?;VALVE?;ALARM?');
  check('AUTO 5 mbar stable', s.reg() === 3, 'regulation ' + s.reg());
  check('poll in AUTO', /^PRESSURE [45]\.\d{3} mbar;SETPOINT 5\.0 mbar;MODE AUTO;VALVE \d+\.\d\d %;ALARM NONE\r\n$/.test(d), d);
  shot(s, 'auto');
  check('control tuning saves in AUTO', s.save(CFG.KP, 300) && s.setting(CFG.KP) === 300 &&
    s.cmd('MODE?') === 'MODE AUTO\r\n', s.why());
}
{ // A large setpoint change in AUTO applies at once from the SP keypad: no question.
  const s = homed(load());
  s.cmd('SETPOINT 5;MODE AUTO'); s.run(5000);
  s.tap(121, 184); s.tap(40, 239); s.tap(110, 183); s.tap(320, 212);   // SP box, 1, 5, Set
  s.run(500);
  shot(s, 'sp-change');
  const st = s.cmd('SETPOINT?;MODE?');
  check('SP 5 to 15 mbar in AUTO applies without a question', st === 'SETPOINT 15.0 mbar;MODE AUTO\r\n', st);
}
{ // The motor supply is not sensed: counted moves can continue without physical motion.
  const s = homed(load());
  s.cmd('SETPOINT 5;MODE AUTO'); s.run(400000);
  s.set(12, 0);
  const needle = s.get(5), count = s.get(30);
  s.cmd('SETPOINT 8'); s.run(5000);
  check('motor power loss does not directly end AUTO', s.cmd('MODE?;ALARM?') === 'MODE AUTO;ALARM NONE\r\n');
  check('unpowered motor stays still while the step count advances', s.get(5) === needle && s.get(30) !== count);
}
{ // W-2: the needle slips in AUTO, so the pressure does not follow. AUTO ends,
  // nothing latches, the strip says so until OK, and Start AUTO is allowed again without a reset.
  const s = oldGains(homed(load()));
  s.cmd('SETPOINT 5'); s.cmd('MODE AUTO'); s.run(300000);
  s.set(6, 0); s.cmd('SETPOINT 8');
  s.run(600000);
  const ev = events(s, 10), st = s.cmd('MODE?;ALARM?');
  check('W-2 no pressure response: MANUAL, a warning, no fault', /W-2,No pressure response in 5 windows of 30 s/.test(ev) &&
    /MODE,MANUAL after W-2/.test(ev) && !/F-\d,/.test(ev) &&
    /^MODE MANUAL;ALARM W-2,1,No pressure response: switched to MANUAL\r\n$/.test(st), ev + st);
  shot(s, 'w02');
  s.set(6, 1);
  check('W-2: Start AUTO again is allowed, and it clears the strip', s.cmd('MODE AUTO;MODE?;ALARM?') === 'MODE AUTO;ALARM NONE\r\n', s.cmd('MODE?;ALARM?'));
  s.cmd('MODE MANUAL');
}
{ // No convergence timeout: the rotary pump stops in AUTO, the pressure stays off the setpoint, and AUTO stays on
  // with nothing raised.
  const s = oldGains(homed(load()));
  s.cmd('SETPOINT 5'); s.cmd('MODE AUTO'); s.run(300000);
  s.set(4, 0);
  s.run(1900000);
  const ev = events(s, 10), st = s.cmd('MODE?;ALARM?');
  check('pump off for 32 min: still AUTO, nothing raised', !/W-\d,/.test(ev) && st === 'MODE AUTO;ALARM NONE\r\n', ev + st);
  shot(s, 'pump-off-auto');
}
{ // A move that runs past its time is stopped and logged, and that is all: no warning, nothing latched, the next
  // move runs.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000);
  s.set(24, 1);                       // the step interrupt does not run
  s.cmd('VALVE SET 31', 1000); s.run(20000);
  const ev = events(s, 5), st = s.cmd('MODE?;VALVE MOVING?;ALARM?');
  check('move timed out in MANUAL: stopped, a SYS event, no warning', /SYS,Move stopped at 30\.00 % \(count \d+\): it ran past its time/.test(ev) &&
    !/[WF]-\d,/.test(ev) && st === 'MODE MANUAL;VALVE MOVING 0;ALARM NONE\r\n', ev + st);
  s.set(24, 0);
  check('move timed out: the next move runs', s.cmd('VALVE SET 31', 60000) === '' && s.cmd('VALVE?') === 'VALVE 31.00 %\r\n', s.cmd('VALVE?'));
  s.cmd('SETPOINT 5;MODE AUTO', 1000); s.run(5000);
  s.set(24, 1); s.run(120000);
  const ev2 = events(s, 30), st2 = s.cmd('MODE?;ALARM?');
  const said = (ev2.match(/ran past its time/g) || []).length;
  check('move timed out in AUTO: AUTO stays on, said once', st2 === 'MODE AUTO;ALARM NONE\r\n' && said === 2, said + ' ' + ev2 + st2);
  s.set(24, 0);
}
{ // A move timed out while homing: the homing ends, not homed; homing again works.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.set(24, 1);
  s.home(200000);
  const ev = events(s, 6), st = s.cmd('MODE?;ALARM?');
  check('move timed out while homing: homing aborted, NOT HOMED, nothing raised', /SYS,Move stopped \(count \d+\)/.test(ev) &&
    /REF,Homing aborted \(move timed out\)/.test(ev) && st === 'MODE NOT HOMED;ALARM NONE\r\n', ev + st);
  s.set(24, 0);
  s.home();
  check('move timed out while homing: homes at the next try', /Homed in/.test(events(s, 10)) && s.cmd('MODE?') === 'MODE MANUAL\r\n', events(s, 10));
}
{ // A saved record of another layout version is not read: the events say so, and the factory settings load.
  const crc32 = b => { let c = ~0; for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xEDB88320 & -(c & 1)); } return ~c >>> 0; };
  const t = load(); t.boot(); t.run(3000);
  const saved = t.save(CFG.JOG, 250), f = t.flash(), A = 0x7fd000;
  const dv = new DataView(f.buffer, f.byteOffset), size = dv.getUint32(A + 8, true);
  check('a save writes the record to slot A', saved && dv.getUint32(A + 4, true) === 13 && size > 200 && size < 4096,
    [saved, t.why(), dv.getUint32(A + 4, true), size].join());
  const u = load(); u.boot(f); u.run(3000);
  check('the saved record loads after a power cycle', /Settings saved [-0-9: ]+ loaded \(slot A\)/.test(events(u, 20)) && u.setting(CFG.JOG) === 250,
    events(u, 20) + u.setting(CFG.JOG));
  dv.setUint32(A + 4, 12, true);
  dv.setUint32(A + size - 4, crc32(f.subarray(A, A + size - 4)), true);
  const v = load(); v.boot(f); v.run(3000);
  const ev = events(v, 20);
  check('a record of another version: not read, said, factory settings',
    /Saved record not used \(slot A\): not a settings record of this version/.test(ev) && /No saved settings, factory defaults/.test(ev) &&
    v.setting(CFG.JOG) === 100, ev + v.setting(CFG.JOG));
}
{ // A hot controller chip is no alarm.
  const s = homed(load()); s.set(13, 57); s.run(3000);
  check('no W-3 on a hot chip', !/W-3/.test(events(s, 5)) && s.cmd('ALARM?') === 'ALARM NONE\r\n', events(s, 5) + s.cmd('ALARM?'));
}
{ // Settings and the saved position survive a power cycle (serial flash).
  const s = homed(load());
  s.cmd('VALVE SET 10', 20000); const saved = s.save(CFG.KP, 300); s.run(5000);
  const flash = s.flash(), needle = s.get(5);
  const t = load(); t.x.sim_bench_init(); t.boot(flash); t.set(5, needle); t.run(3000);
  check('settings kept over a power cycle', saved && t.setting(CFG.KP) === 300, s.why() + ' KP ' + t.setting(CFG.KP));
  check('valve position offered after a power cycle', Math.abs(t.offer() - 6605) <= 1, 'offer ' + t.offer());
  // The question needs an answer: a tap round it, on the strip or on the power icon leaves it.
  t.turnOn(5000); const asked = t.popup();
  t.tap(455, 100); t.tap(100, 240); t.tap(24, 20);
  check('saved position question stays on a tap round it', asked && t.popup() && Math.abs(t.offer() - 6605) <= 1,
    [asked, t.popup(), t.offer()].join());
}
{ // Limit override is a two-finger deadman: held alone it does nothing; held, a second
  // finger on Close closes past a pressed switch, on Open opens; either finger lifting stops it.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.tap(415, 184); s.tap(84, 128);                                      // Menu > Motor: its Jog tab
  s.set(7, 1); s.run(500);                                              // switch stuck pressed
  s.tap(64, 156, 1000);
  const c0 = s.get(30), blocked = c0 === 0;                             // Close alone refused
  // Close, Limit override and Open, 109 x 104 at x 10, 127 and 244.
  const over = (d) => s.x.sim_touch(181, 156, d), second = (px, d) => s.x.sim_touch2(px, 156, d);
  over(1); s.run(2000); const alone = s.get(30);
  check('limit override alone does nothing', blocked && alone === c0 && !s.busy(), 'count ' + alone);
  s.x.sim_touch2(120, 72, 1); s.run(1000); s.x.sim_touch2(120, 72, 0); s.run(200);   // second finger on the tabs
  check('second finger elsewhere does nothing', s.get(30) === c0, 'count ' + s.get(30));
  second(64, 1); s.run(3000); const held = s.get(30);
  // At the Jog speed (100 steps/s), not the run speed (400): about 300 steps in 3 s.
  check('override + Close closes past the switch', held < -250 && held > -400, 'count ' + held);
  shot(s, 'override');
  second(64, 0); s.run(300); const rel = s.get(30); s.run(1000);
  check('lifting the second finger stops at once', s.get(30) === rel && !s.override(), s.get(30) + ' vs ' + rel);
  second(64, 1); s.run(2000); const again = s.get(30);
  over(0); s.run(300); const rel2 = s.get(30); s.run(1000);
  check('lifting the override finger stops the close', again < rel - 150 && s.get(30) === rel2, again + ' ' + rel2 + ' ' + s.get(30));
  second(64, 0); s.run(300);
  s.x.sim_touch(64, 156, 1); s.run(1000); s.x.sim_touch(64, 156, 0); s.run(500);
  check('Close alone still refused after', s.get(30) === rel2, 'count ' + s.get(30));
  over(1); s.run(200); second(298, 1); s.run(2000); const opened = s.get(30);
  second(298, 0); over(0); s.run(2000);
  // Close and Open are hold to run only: Open moves while pressed and ramps down on release.
  const p0 = s.get(30); s.x.sim_touch(298, 156, 1); s.run(2000); const p1 = s.get(30); s.x.sim_touch(298, 156, 0); s.run(3000);
  const p2 = s.get(30); s.run(1000);
  shot(s, 'motor-override-move');
  check('Motor > Jog: Open runs while held and stops on release', p1 > p0 + 150 && p1 < p0 + 300 && p2 >= p1 && p2 < p1 + 400 && s.get(30) === p2 && !s.busy(),
    [p0, p1, p2, s.get(30)].join());
  check('override + Open opens', opened > rel2 + 150, opened + ' vs ' + rel2);
}
{ // The grammar of the screen texts with their values: "1 step", the full travel time without a zero part ("25 s",
  // "2 min"), "Capturing 1 of 5" for the first reading, one colon in the capture refusal, and DD/MM/YY dates in the
  // Events lines.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const dump = () => { s.x.sim_textfit_all(1); const d = str(s.x.sim_textfit()); s.x.sim_textfit_all(0); return d; };
  const has = t => dump().includes('"' + t + '"');
  s.save(CFG.JOG, 16);
  s.tap(415, 184); s.tap(84, 128);                                      // Menu > Motor: its Jog tab
  const jog = (x, ms) => { s.x.sim_touch(x, 156, 1); s.run(ms); s.x.sim_touch(x, 156, 0); s.run(1500); return s.get(30); };
  const at0 = has('0 steps. Valve idle'), p1 = jog(298, 30), one = has('1 step. Valve idle'), p2 = jog(298, 60), two = has('2 steps. Valve idle');
  let pm = p2; for (let i = 0; i < 10 && pm > -1; i++) pm = jog(64, 30);
  const minus = has('-1 step. Valve idle');
  check('Motor > Jog strip: 0 steps, 1 step, 2 steps, -1 step', at0 && p1 === 1 && one && p2 === 2 && two && pm === -1 && minus,
    [at0, p1, one, p2, two, pm, minus].join());
  s.tap(177, 72); s.run(600);                                           // Settings
  const t165 = has('Full travel 2 min 45 s at this speed');
  s.x.sim_apply(CFG.POSMAX, 10000); s.run(600); const t25 = has('Full travel 25 s at this speed');
  s.x.sim_apply(CFG.POSMAX, 48048); s.run(600); const t120 = has('Full travel 2 min at this speed');
  check('Motor > Settings strip: full travel as "2 min 45 s", "25 s" and "2 min"', t165 && t25 && t120, [t165, t25, t120].join());
  s.x.sim_apply(CFG.POSMAX, 66048); s.setClosed();
  s.tap(66, 20); s.tap(396, 128);                                       // Back, Pressure sensor
  s.tap(181, 183); s.tap(357, 146, 600);                                // Air point > Capture, at vacuum
  const why = /"Not started: not at room air, vent the sensor/.test(dump().replace(/ \/ /g, ' '));   // its line break   // the dump cuts a text at 48 characters
  s.tap(290, 239); s.run(300);                                          // OK
  s.tap(181, 127); s.tap(357, 146, 300);                                // Vacuum point > Capture
  const first = has('Capturing 1 of 5'); let zero = false, last = '';
  for (let i = 0; i < 40 && /"Capturing/.test(dump()); i++) { zero = zero || /"Capturing 0 of/.test(dump()); last = dump().match(/"Capturing (\d) of 5"/)[1]; s.run(200); }
  check('Pressure sensor: one colon in the refusal; the capture counts 1 of 5 to 5 of 5', why && first && !zero && last === '5', [why, first, zero, last].join());
  const b = Buffer.from('Settings saved 2026-10-24 14:02:05 loaded from A\0');
  new Uint8Array(s.x.memory.buffer, s.x.sim_text_buf(), b.length).set(b);
  const d1 = str(s.x.sim_dates());
  check('Events lines: dates as DD/MM/YY HH:MM', d1 === 'Settings saved 24/10/26 14:02 loaded from A', d1);
}
{ // Jog speed (Motor > Settings > Jog): Close and Open run at it, not at the run speed.
  const s = homed(load()); s.cmd('VALVE SET 30', 60000);
  const saved = s.save(CFG.JOG, 400);
  s.tap(415, 184); s.tap(84, 128);                                      // Menu > Motor: its Jog tab
  const p0 = s.get(30); s.x.sim_touch(298, 156, 1); s.run(2000); const p1 = s.get(30); s.x.sim_touch(298, 156, 0); s.run(3000);
  check('Jog speed 400: Open runs at it (about 700 steps in 2 s, 200 at the factory 100)', saved && p1 - p0 > 500,
    [saved, s.why(), p0, p1].join());
}
{ // W-1 and Limit override: while the override is held the question is not asked, so it cannot take the
  // buttons from under two fingers; a press still there when the override ends is asked then.
  const s = homed(load()); s.cmd('VALVE SET 30', 60000);
  s.set(7, 1); s.run(1000);                                             // the switch reads pressed at 30 %
  const asked = s.popup() && /W-1/.test(s.cmd('ALARM?'));
  s.tap(160, 147);                                                      // Skip
  s.tap(415, 184); s.tap(84, 128);                                      // Menu > Motor: its Jog tab
  const over = (d) => s.x.sim_touch(181, 156, d), second = (px, d) => s.x.sim_touch2(px, 156, d);
  const c0 = s.get(30);
  over(1); s.run(300); second(64, 1); s.run(1000);
  s.set(7, 0); s.run(300); s.set(7, 1); s.run(1700);                    // released and pressed again: a new press, mid-hold
  const held = s.get(30), quiet = !s.popup() && s.cmd('ALARM?', 100) === 'ALARM NONE\r\n';
  second(64, 0); over(0); s.run(1000);
  const after = s.popup() && /W-1/.test(s.cmd('ALARM?'));
  check('W-1 waits while Limit override is held, and asks when it ends', asked && held < c0 - 200 && quiet && after,
    [asked, c0, held, quiet, after, s.cmd('ALARM?').trim()].join(' '));
}
{ // A lone captured point through a settings change: the vacuum point stays while the sensor is vented for the
  // air point, and a settings change does not put the saved calibration back.
  const s = load(); s.boot(); s.run(3000);
  s.set(11, 0); s.run(5000);
  const v = s.cal(1, 0);
  const changed = s.x.sim_apply(CFG.JOG, 300) && s.save(CFG.KP, 50);   // an edit, then a Save
  s.set(11, 1); s.run(60000);
  const a = s.cal(2, 1013), ev = events(s, 4);
  check('a lone vacuum point is kept through an edit and a save: the air point completes the calibration',
    v && changed && a && /Bench cal applied/.test(ev) && !/Vacuum point next/.test(ev), [v, changed, a, s.why()].join() + '\n' + ev);
}
{ // The room sensor lost during an air point capture: its row says so, as for the pressure signal.
  const s = load(); s.boot(); s.run(3000);
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const dump = () => { s.x.sim_textfit_all(1); const d = str(s.x.sim_textfit()); s.x.sim_textfit_all(0); return d; };
  s.set(11, 1); s.set(19, 1008.4); s.run(5000);
  s.tap(415, 184); s.tap(396, 128);                                     // Menu > Pressure sensor
  s.tap(181, 183); s.tap(357, 146, 300);                                // Air point > Capture, at the room sensor's reading
  const capturing = dump().includes('"Capturing');
  s.set(20, 0); s.run(4000);                                            // the room sensor stops answering
  const said = dump().includes('"Capture cancelled: no barometer reading"');
  check('room sensor lost mid-capture: "Capture cancelled: no barometer reading" on the Air point row',
    capturing && said, [capturing, said].join() + '\n' + (dump().match(/"[^"]*(Captur|captured)[^"]*"/g) || []).join(' '));
}
{ // Pressure 21: the air point at the room sensor's reading.
  const s = load(); s.boot(); s.run(3000);
  s.set(11, 0); s.run(3000); s.cal(1, 0);
  s.set(11, 1); s.set(19, 1008.4); s.run(3000);
  const a = s.cal(2, -1), ev = events(s, 4);
  check('air point from the room sensor', a && /Air point [\d.]+ mV\/V at 1008\.40 mbar from the room sensor/.test(ev), s.why() + ev.slice(-400));
  // No room sensor: the air point needs a typed reading, BAROMETER? says NONE.
  const u = load(); u.boot(); u.set(20, 0); u.run(3000); u.set(11, 1); u.run(3000);
  check('BAROMETER NONE without the sensor', u.cmd('BAROMETER?', 1000) === 'BAROMETER NONE\r\n', u.cmd('BAROMETER?', 1000));
  const r = u.cal(2, -1, 2000);
  check('room air point refused without the sensor', !r && /no barometer reading/i.test(u.why()), u.why());
  check('typed air point still works', u.cal(2, 1013), u.why());
}
{ // The remote set: the reply rules of the handover page.
  const s = homed(load());
  const one = c => s.cmd(c, 1000);
  // R1, R2, R5: no echo; a query gives one line starting with its own words.
  const qs = ['PRESSURE?', 'SETPOINT?', 'MODE?', 'VALVE?', 'VALVE MOVING?', 'BAROMETER?', 'ALARM?', 'TIME?'];
  const bad = qs.map(q => [q, one(q)]).filter(([q, r]) => !(r.startsWith(q.slice(0, -1) + ' ') && /^[^\r\n]*\r\n$/.test(r)))
    .map(([q, r]) => q + ' -> ' + JSON.stringify(r));
  check('queries: one line, own words first', bad.length === 0, bad.join('\n     '));
  // R2: set commands silent when accepted.
  const sets = ['SETPOINT 5', 'MODE MANUAL', 'VALVE SET 30', 'STOP', 'ALARM OK', 'ALARM RESET', 'TIME SET 2026-10-27 12:00:00'];
  const loud = sets.map(c => [c, s.cmd(c, c.startsWith('VALVE') ? 60000 : 1000)]).filter(([, r]) => r !== '').map(([c, r]) => c + ' -> ' + JSON.stringify(r));
  check('set commands silent', loud.length === 0, loud.join('\n     '));
  check('TIME SET then TIME?', /^TIME 2026-10-27 12:00:\d\d\r\n$/.test(one('TIME?')), one('TIME?'));
  // R4, R6, R7: a query confirms the value taken; fixed decimals; chaining.
  check('SETPOINT 5.04;SETPOINT? gives 5.0', one('SETPOINT 5.04;SETPOINT?') === 'SETPOINT 5.0 mbar\r\n', one('SETPOINT?'));
  const poll = one('PRESSURE?;SETPOINT?;MODE?;VALVE?;ALARM?');
  check('chained poll', /^PRESSURE \d+\.\d{3} mbar;SETPOINT 5\.0 mbar;MODE MANUAL;VALVE 30\.00 %;ALARM NONE\r\n$/.test(poll), poll);
  check('BAROMETER? in the simulator', one('BAROMETER?') === 'BAROMETER 1013.20 mbar\r\n', one('BAROMETER?'));
  // Not commands: NETWORK?, PID? and PID SET (PIDS? and PIDS SET are).
  const gone = ['NETWORK?', 'PID?', 'PID SET 3,0.5'].map(c => [c, one(c)]).filter(([, r]) => !/^ERR 100 /.test(r)).map(([c, r]) => c + ' -> ' + JSON.stringify(r));
  check('NETWORK?, PID? and PID SET are unknown commands', gone.length === 0, gone.join('\n     '));
  check('*IDN?', /^MercShore,iPC,[0-9A-F]{24},\d+\.\d+\.\d+\r\n$/.test(one('*IDN?')), one('*IDN?'));
  // EVENT?: the count line, then that many lines, newest first.
  const ev = s.cmd('EVENT? 3', 2000).split('\r\n');
  check('EVENT? 3', ev[0] === 'EVENT 3' && ev.slice(1, 4).every(l => /^\d+\.\d,[A-Z0-9-]+,/.test(l)) && ev.length === 5 && ev[4] === '',
    ev.join(' | '));
  check('EVENT? defaults to 1', s.cmd('EVENT?', 2000).split('\r\n').length === 3);
  // The next line waits while an EVENT? reply goes out, as on the board (main.c), so its answer is not among the
  // event lines.
  s.out = ''; s.send('EVENT? 10'); s.send('MODE?'); s.run(2000);
  const held = s.out.split('\r\n');
  check('EVENT? then MODE?: the MODE reply after the 10 event lines', held[0] === 'EVENT 10' && /^MODE /.test(held[11]) &&
    held.slice(1, 11).every(l => /^\d+\.\d,[A-Z0-9-]+,/.test(l)), held.slice(0, 13).join(' | '));
  // R8: ERR <code> <text>, the first refusal ends the line.
  const errs = [['NO SUCH', 100], ['SETPOINT x', 101], ['SETPOINT 2000', 102], ['X'.repeat(97), 103], ['MODE?;EVENT? 2', 104],
    ['EVENT? 0', 102], ['VALVE SET 101', 102], ['VALVE SET', 101], ['MODE', 101], ['TIME SET 2026-13-01 00:00:00', 101],
    ['PRESSURE? 5', 100], ['pressure?', 100], ['SETPOINT', 101], ['SETPOINTS 5', 100], ['EVENT?5', 100],
    // Four-letter names are not commands.
    ['PRES?', 100], ['PRES SP 5', 100], ['PRES SP?', 100], ['VALV?', 100], ['VALV SET 5', 100], ['ALRM?', 100], ['ALRM OK', 100],
    ['EVNT?', 100], ['BARO?', 100], ['NETW?', 100]];
  const wrong = errs.map(([c, n]) => [c, n, one(c)]).filter(([c, n, r]) => !new RegExp('(^|;)ERR ' + n + ' [A-Z][^;\\r\\n]*\\r\\n$').test(r))
    .map(([c, n, r]) => c.slice(0, 20) + ' -> ' + JSON.stringify(r) + ' (want ' + n + ')');
  check('error codes 100-104', wrong.length === 0, wrong.join('\n     '));
  check('first refusal ends the line', one('SETPOINT 2000;SETPOINT?') === 'ERR 102 Outside 0 to 1034.2 mbar\r\n', one('SETPOINT 2000;SETPOINT?'));
  // The page's example: AUTO, then VALVE SET refused until MANUAL.
  check('MODE AUTO;MODE?', one('MODE AUTO;MODE?') === 'MODE AUTO\r\n', one('MODE?'));
  check('ERR 202 in AUTO', one('VALVE SET 40;VALVE MOVING?') === 'ERR 202 Switch to MANUAL first\r\n');
  check('MODE MANUAL;VALVE SET 40;VALVE MOVING?', one('MODE MANUAL;VALVE SET 40;VALVE MOVING?') === 'VALVE MOVING 1\r\n');
  s.run(30000);
  check('STOP in AUTO ends AUTO', (one('MODE AUTO'), one('STOP;MODE?')) === 'MODE MANUAL\r\n');
  // 201 while homing, 204 not homed, 206 no target, 210 cause present.
  s.home(200);
  check('MODE HOMING, moving', one('MODE?;VALVE MOVING?') === 'MODE HOMING;VALVE MOVING 1\r\n', one('MODE?'));
  check('ERR 201 while homing', one('VALVE SET 10') === 'ERR 201 Homing, STOP to cancel\r\n', one('VALVE SET 10'));
  one('STOP');
  const u = load(); u.boot(); u.run(2000);
  check('ERR 204 not homed', u.cmd('SETPOINT 5;MODE AUTO;MODE?') === 'ERR 204 Home the valve first\r\n', u.cmd('MODE?'));
  check('VALVE SET not homed', u.cmd('VALVE SET 40') === 'ERR 204 Home the valve first\r\n', u.cmd('VALVE SET 40'));
  const t = homed(load());
  check('ERR 206 no setpoint', t.cmd('MODE AUTO') === 'ERR 206 Set a setpoint first\r\n');
  t.set(8, 0); t.run(5000);
  check('ERR 210 cause present', /^ERR 210 Not reset, cause still present: F-1\r\n$/.test(t.cmd('ALARM RESET')), t.cmd('ALARM RESET'));
  check('ALARM? fault flag 0 while its cause is present', /^ALARM F-1,0,/.test(t.cmd('ALARM?')), t.cmd('ALARM?'));
  t.set(8, 1); t.run(5000);
  check('ALARM? fault flag 1 once it can reset', /^ALARM F-1,1,/.test(t.cmd('ALARM?')), t.cmd('ALARM?'));
  check('ALARM RESET clears it', t.cmd('ALARM RESET;ALARM?') === 'ALARM NONE\r\n', t.cmd('ALARM?'));
  check('nothing sent unasked over a run', (t.cmd('SETPOINT 5;MODE AUTO', 60000), t.out === ''), t.out);
}
{ // Network control switch: On by default; Off, the link takes queries only; On from the screen, it takes sets.
  const d = load(); d.boot(); d.run(1000);   // boot() leaves the switch as the firmware starts it
  check('switch On by default', d.cmd('SETPOINT 5;SETPOINT?') === 'SETPOINT 5.0 mbar\r\n', d.cmd('SETPOINT?'));
  const s = load(); s.boot(undefined, { remote: false }); s.set(5, 40000); s.run(2000);
  check('switch Off: SETPOINT refused', s.cmd('SETPOINT 5') === 'ERR 211 Read only over the network\r\n', s.cmd('SETPOINT 5'));
  check('switch Off: STOP refused', s.cmd('STOP') === 'ERR 211 Read only over the network\r\n');
  check('switch Off: queries answer', s.cmd('MODE?;SETPOINT?') === 'MODE NOT HOMED;SETPOINT NONE\r\n', s.cmd('MODE?;SETPOINT?'));
  s.tap(415, 184); s.tap(84, 184); s.tap(357, 128);   // Menu, System (bottom left), Network control (second row right)
  check('switch On from the screen: SETPOINT taken', s.cmd('SETPOINT 5;SETPOINT?') === 'SETPOINT 5.0 mbar\r\n', s.cmd('SETPOINT?'));
  check('switch On is a NET event', /NET,Network control on \(screen\)/.test(events(s, 5)), events(s, 5));
  shot(s, 'system');
  s.tap(357, 128);
  check('switch Off again: refused', s.cmd('SETPOINT 6') === 'ERR 211 Read only over the network\r\n');
  // The switch saves as it is set: Off is still Off after a power cycle.
  { const t = load(); t.x.sim_bench_init(); t.boot(s.flash()); t.set(5, 40000); t.run(2000);
    check('Network control Off from the screen is saved at once: Off after a power cycle',
      t.cmd('SETPOINT 6') === 'ERR 211 Read only over the network\r\n' && /Settings saved/.test(events(s, 8)), t.cmd('SETPOINT 6') + events(s, 8)); }
  check('switch Off: PIDS SET and TABLE SET refused, PIDS? and TABLE? answer',
    s.cmd('PIDS SET 2,3,0.5') === 'ERR 211 Read only over the network\r\n' &&
    s.cmd('TABLE SET 6,10,14,15') === 'ERR 211 Read only over the network\r\n' &&
    s.cmd('PIDS?;TABLE?') === 'PIDS 1,0.06,0.60,0.00,0.12,0.82,0.00,0.80,0.94,0.00,0.40,0.25,0.00;TABLE 6.0,10.0,14.0,15\r\n',
    s.cmd('PIDS?;TABLE?'));
}
{ // Homing return: its own setting, the step 4 move off the switch; 0 % is there.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  check('return set to 150', s.save(CFG.RETURN, 150) && s.setting(CFG.RETURN) === 150, s.why());
  s.home();
  const ev = events(s, 3);
  check('homed with the switch 150 steps below 0 %', /Homed in \d+ s \(switch at count -150, 0 % at count 0\)/.test(ev) && s.get(5) === 300, ev + ' needle ' + s.get(5));
}
// Shut down and OFF (to 0 %, not onto the switch, and every start OFF):
// the valve goes to 0 %, coils off; moves refused until a
// double tap on the screen. The next start is OFF with the count kept, if
// the switch reads released.
const px = (s, X, Y) => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[Y * 480 + X];
const NEEDLE0 = 800;   // the needle at 0 % after homing: 400 full steps above the seat
let parkedFlash;
{ // Every start is OFF: the black screen, nothing moves, a double tap turns it on.
  const s = load(); s.boot(undefined, { off: true }); s.set(5, 40000); s.run(2000);
  check('start: OFF', s.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE NONE\r\n', s.cmd('MODE?;VALVE?'));
  check('start: black screen', px(s, 10, 10) === 0 && px(s, 240, 136) === 0, px(s, 10, 10));
  check('start: moves refused', s.cmd('VALVE SET 10') === 'ERR 213 Controller is off\r\n', s.cmd('VALVE SET 10'));
  check('start: BOOT event says OFF', /BOOT,Power on: Off, valve not homed/.test(events(s, 5)), events(s, 5));
  shot(s, 'boot-off');
  s.tap(240, 136, 100); s.tap(240, 136, 150);
  check('turning on: the logo on white', px(s, 10, 10) === 0xFFFF, px(s, 10, 10));
  shot(s, 'turning-on');
  s.run(3000);
  check('double tap at start: NOT HOMED', s.cmd('MODE?') === 'MODE NOT HOMED\r\n', s.cmd('MODE?'));
  shot(s, 'turned-on');
}
{ // System: IP address & mask and MAC, Network control and Date and time, Firmware, Up time and Power.
  // Power asks the power icon's question (Power options); its Restart acts at once, with no second question.
  const hdr = (s) => Buffer.from(new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 40).slice().buffer).toString('base64');
  const s = homed(load());
  s.tap(415, 184); s.tap(84, 184); const sys = hdr(s);
  s.tap(415, 184); const shutQ = s.popup(); shot(s, 'system-power');
  s.tap(318, 165, 3000);
  check('System > Power: the Power options question, then shutting down', shutQ && /^MODE (SHUTDOWN|OFF)/.test(s.cmd('MODE?')),
    [shutQ, s.cmd('MODE?')].join());
  const r = homed(load());
  r.tap(415, 184); r.tap(84, 184); r.tap(415, 184); r.tap(200, 165); const restartQ = r.popup();
  r.run(500); const msg = r.popup();
  check('System > Power > Restart: no second question', !restartQ && !msg, [restartQ, msg].join());
  const n = homed(load());
  n.tap(415, 184); n.tap(84, 184); n.tap(120, 72); const ip = hdr(n); shot(n, 'system-ip');
  check('System: IP address & mask on the first row left opens its page', ip !== sys);
}
{ // System without "Static IP address", Date and time left of Network control, Power in Start AUTO's dark blue;
  // Diagnostics columns 116 px (room for Connected); Home's status line without the Disable switch range; the
  // sparkline 139 px wide.
  const s = homed(load());
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const dump = () => { s.x.sim_textfit_all(1); const d = str(s.x.sim_textfit()); s.x.sim_textfit_all(0); return d; };
  const px = (x, y) => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[y * 480 + x];
  const blue = px(30, 170);                                   // Start AUTO on Home
  s.tap(415, 184); s.tap(84, 184); s.run(500); const sys = dump();
  const sysOk = !/Static IP/.test(sys) && /"Date and time" \| text \d+x\d+, box \d+x\d+ at 22,/.test(sys) &&
    /"Network control" \| text \d+x\d+, box \d+x\d+ at 256,/.test(sys) && px(366, 165) === blue;
  s.tap(22, 20); s.tap(415, 184); s.tap(396, 184); s.run(500); const dg = dump();   // Home, Menu > Diagnostics
  const diagOk = /"No client" \| text \d+x17, box 116x17/.test(dg);
  s.tap(22, 20);
  s.tap(415, 184); s.tap(240, 128); s.tap(291, 72); s.tap(273, 184); s.tap(22, 20); s.run(1000);   // Disable switch on, Home
  const hm = dump(), homeOk = !/Limit switch disabled/.test(hm) && /"Valve idle"/.test(hm);
  s.run(300000); let x0 = 480;                                // 5 min of data: the line from the sparkline's left edge
  for (let y = 222; y < 262; y++) for (let x = 150; x < 340; x++) if (px(x, y) === px(338, 252) && x < x0) x0 = x;
  check('System strip, order and Power colour; Diagnostics 116 px columns; no Disable switch on the status line; sparkline 139 px',
    sysOk && diagOk && homeOk && x0 <= 202, [sysOk, diagOk, homeOk, x0].join());
  shot(s, 'home-spark-0-17-18');
}
{ // Save, Undo and Reset to factory. Save and Undo act in one tap; Undo asks only when something in use would go.
  const s = homed(load());
  const screen = () => { const m = new Uint8Array(s.x.memory.buffer); let p = s.x.sim_screen(), e = p; while (m[e]) e++;
    return Buffer.from(m.subarray(p, e)).toString(); };
  const home = () => { if (screen() !== 'Home') s.tap(22, 20); }, menu = () => { home(); s.tap(415, 184); };
  const kp = d => { s.tap(65, 184); s.tap(230, 183); s.tap(230, 239); s.tap(d === 5 ? 110 : 170, 183); s.tap(320, 212); };   // KP 1: 0 . d Set
  menu(); s.tap(396, 72); s.run(300);                                  // PID
  kp(5);
  const inUse = s.setting(CFG.KP);                                     // in use at once, not saved
  shot(s, 'save-in-use');
  s.tap(415, 184); const undoAsk = s.popup();                          // Undo: asks
  shot(s, 'save-undo-pid-ask');
  s.tap(320, 147); s.run(500);
  shot(s, 'save-undone');
  s.tap(415, 128);                                                     // Save: nothing to save
  check('a keypad value is in use at once; Undo asks, then puts the last save back; nothing is saved',
    inUse === 50 && undoAsk && s.setting(CFG.KP) === 6 && !/Settings saved/.test(events(s, 5)),
    [inUse, undoAsk, s.setting(CFG.KP)].join());
  kp(5); s.tap(415, 128); const noQ2 = !s.popup();
  shot(s, 'save-saved');
  check('Save applies and saves at once, no question', noQ2 && s.setting(CFG.KP) === 50 && /Settings saved/.test(events(s, 3)),
    [noQ2, s.setting(CFG.KP)].join() + events(s, 3));
  // Reset to factory on the Menu: asks; Cancel leaves everything; Reset puts factory in use and saves it.
  menu(); s.tap(240, 184); const asked = s.popup();
  shot(s, 'reset-ask');
  s.tap(161, 155); const cancelled = !s.popup() && s.setting(CFG.KP) === 50;
  s.tap(240, 184); s.tap(319, 155);
  shot(s, 'reset-done');
  const t = load(); t.x.sim_bench_init(); t.boot(s.flash()); t.set(5, 40000); t.run(2000);
  check('Reset to factory: asks, Cancel changes nothing, Reset puts the factory settings in use and saves them',
    asked && cancelled && !s.popup() && s.setting(CFG.KP) === 6 && /Reset to factory/.test(events(s, 4)) && t.setting(CFG.KP) === 6,
    [asked, cancelled, s.popup(), s.setting(CFG.KP), t.setting(CFG.KP)].join() + events(s, 4));
  // A new calibration is in use at once and not saved. Undo on its page asks, then puts the saved one back.
  let calWhy = '';
  const calibrate = () => { s.set(11, 0); s.set(40, 0.02); s.run(5000); const a = s.cal(1, 0); calWhy = s.why() + ' ' + s.cmd('PRESSURE?'); s.set(11, 1); s.run(60000); const b = s.cal(2, 1009.8); return a && b; };
  const air = () => parseFloat(s.cmd('PRESSURE?').split(' ')[1]);
  const nominal = (s.set(11, 1), s.run(60000), air());
  const cal1 = calibrate(), new1 = air();
  menu(); s.tap(396, 128); s.run(500);                                 // Pressure sensor
  shot(s, 'save-cal-unsaved');
  s.tap(415, 184); const undoQ = s.popup();                            // Undo: asks
  shot(s, 'save-undo-ask');
  s.tap(320, 147); s.run(1500);
  check('Undo with a new calibration in use asks, then puts the saved one back',
    cal1 && Math.abs(new1 - 1009.8) < 0.5 && undoQ && !s.popup() && Math.abs(air() - nominal) < 0.5 && Math.abs(nominal - 1009.8) > 5 &&
    /Undo: settings saved/.test(events(s, 4)), [cal1, calWhy, new1, undoQ, s.popup(), air(), nominal].join() + events(s, 4));
  // The header's unsaved icon (left of the remote light) asks to save what is in use.
  const cal2 = calibrate();
  home(); s.run(4000);
  shot(s, 'save-unsaved-icon');
  let iconQ = false;
  for (const x of [287, 325]) { if (!iconQ) { s.tap(x, 20); iconQ = s.popup(); } }
  shot(s, 'save-unsaved-ask');
  s.tap(320, 147); s.run(1500);
  const u = load(); u.x.sim_bench_init(); u.boot(s.flash()); u.set(5, 40000); u.set(11, 1); u.run(60000);
  const kept = parseFloat(u.cmd('PRESSURE?').split(' ')[1]);
  check('the unsaved icon asks to save; Save keeps the new calibration over a power cycle',
    cal2 && iconQ && !s.popup() && Math.abs(kept - 1009.8) < 0.5, [cal2, iconQ, s.popup(), kept].join() + events(s, 4));
  s.set(11, 0); s.run(120000);
  // In AUTO the reset is refused: no question.
  s.cmd('SETPOINT 5;MODE AUTO', 3000);
  menu(); s.tap(240, 184);
  check('Reset to factory in AUTO: refused, no question', !s.popup() && s.cmd('MODE?') === 'MODE AUTO\r\n', s.popup() + s.cmd('MODE?'));
}
{ // Pressure sensor: a captured point gets the draft edge.
  const s = homed(load());
  const px = (x, y) => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[y * 480 + x];
  s.tap(415, 184); s.tap(396, 128); s.run(500);
  shot(s, 'sensor-cal');
  const before = px(10, 126), air0 = px(10, 182);
  s.tap(298, 128); const asked = s.popup(); shot(s, 'sensor-cal-vac-ask');
  s.tap(320, 147); s.run(20000);
  shot(s, 'sensor-cal-new-vac');
  check('Pressure sensor: a new vacuum point gets the dark edge, the air point not', asked && px(10, 126) === 0x1946 &&
    before !== 0x1946 && px(10, 182) === air0, [asked, before.toString(16), px(10, 126).toString(16), px(10, 182).toString(16)].join());
}
{ // Diagnostics: three row cards, Druck, Motor and Board, no Help.
  const s = homed(load());
  s.tap(415, 184); s.tap(396, 184); s.run(500);   // Menu > Diagnostics (bottom right)
  shot(s, 'diagnostics');
  // With the power monitor not answering, Temperature (its own sensor) reads "----" as Supply and
  // Output do, not the last value read.
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const dump = () => { s.x.sim_textfit_all(1); const d = str(s.x.sim_textfit()); s.x.sim_textfit_all(0); return d; };
  const temp = () => (dump().match(/"([^"]*)" \| text \d+x\d+, box 116x17 at 230,184/) || [])[1];
  const live = temp(); s.set(9, 0); s.run(3000); const gone = temp(); s.set(9, 1); s.run(3000); const back = temp();
  check('Diagnostics: Temperature "----" while the power monitor does not answer',
    /^\d+\.\d$/.test(live) && gone === '----' && /^\d+\.\d$/.test(back), [live, gone, back].join());
}
{ // From the screen: the power icon (top left), Shut down, the black screen, OFF, a double tap.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000); s.run(2000);
  s.tap(24, 20); shot(s, 'shutdown-ask');
  s.tap(318, 165, 3000);
  const during = s.cmd('MODE?;VALVE MOVING?');
  check('shut down: to 0 %', during === 'MODE SHUTDOWN;VALVE MOVING 1\r\n', during);
  check('shut down: ERR 213 while moving', s.cmd('VALVE SET 10') === 'ERR 213 Shutting down, STOP to cancel\r\n' &&
    s.cmd('MODE MANUAL') === 'ERR 213 Shutting down, STOP to cancel\r\n', s.cmd('VALVE SET 10'));
  shot(s, 'shutting-down');
  s.run(60000);
  const st = s.cmd('MODE?;VALVE?;VALVE MOVING?');
  check('shut down: OFF at 0 %', st === 'MODE OFF;VALVE 0.00 %;VALVE MOVING 0\r\n', st);
  check('shut down: needle at 0 %, switch not reached, coils off', s.get(5) === NEEDLE0 && s.get(15) === 0,
    'needle ' + s.get(5) + ' coils ' + s.get(15));
  check('OFF: ERR 213', s.cmd('VALVE SET 10') === 'ERR 213 Controller is off\r\n' && s.cmd('MODE AUTO') === 'ERR 213 Controller is off\r\n' &&
    s.cmd('MODE MANUAL') === 'ERR 213 Controller is off\r\n', s.cmd('MODE AUTO'));
  check('OFF: pressure still read', /^PRESSURE [\d.]+ mbar/.test(s.cmd('PRESSURE?')), s.cmd('PRESSURE?'));
  check('shut down event', /OFF,Valve at 0 % \(count 0\): coils off, Off/.test(events(s, 5)), events(s, 5));
  shot(s, 'off');
  parkedFlash = s.flash();
  s.tap(240, 136, 1500);
  check('OFF: one tap does nothing', s.cmd('MODE?') === 'MODE OFF\r\n', s.cmd('MODE?'));
  s.tap(240, 136, 100); s.tap(240, 136, 300);
  s.run(3000);
  check('OFF: a double tap turns it on, MANUAL', s.cmd('MODE?;VALVE?') === 'MODE MANUAL;VALVE 0.00 %\r\n', s.cmd('MODE?;VALVE?'));
  shot(s, 'on-again');
  s.cmd('VALVE SET 5', 15000);
  check('on again: moves', s.cmd('VALVE?') === 'VALVE 5.00 %\r\n', s.cmd('VALVE?'));
}
{ // Close the valve unticked: OFF at once, the valve left open, its count kept.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000); s.run(2000);
  const needle = s.get(5);
  s.tap(24, 20); s.tap(240, 111); shot(s, 'shutdown-leave');
  s.tap(318, 165, 1000);
  check('shut down, valve left: OFF at 30 %', s.cmd('MODE?;VALVE?;VALVE MOVING?') === 'MODE OFF;VALVE 30.00 %;VALVE MOVING 0\r\n' &&
    s.get(5) === needle && s.get(15) === 0, s.cmd('MODE?;VALVE?') + ' needle ' + s.get(5) + ' coils ' + s.get(15));
  check('shut down, valve left: event', /OFF,Shut down, valve left at 30\.00 %: coils off, Off/.test(events(s, 5)), events(s, 5));
  const t = load(); t.boot(s.flash(), { off: true }); t.set(5, needle); t.run(2000);
  check('shut down, valve left: kept at the next start', t.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE 30.00 %\r\n', t.cmd('MODE?;VALVE?'));
  const u = homed(load());
  u.tap(24, 20); u.tap(240, 111); u.tap(240, 111); u.tap(318, 165, 1000);
  check('ticked again: closes', u.cmd('MODE?') === 'MODE OFF\r\n' && /at 0 %: coils off, Off/.test(events(u, 5)), events(u, 5));
}
{ // Power-up after Shut down, the switch released: OFF, the count kept; on, it moves at once.
  for (const r of [0, 1, 2]) {
    const s = load(); s.boot(parkedFlash, { off: true, reset: r }); s.set(5, NEEDLE0); s.run(2000);
    const what = ['power-up', 'watchdog', 'Restart'][r];
    check('start after Shut down (' + what + '): OFF at 0 %', s.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE 0.00 %\r\n' &&
      s.offer() === -2147483648, s.cmd('MODE?;VALVE?') + ' offer ' + s.offer());
    if (r) continue;
    check('start after Shut down: event', /REF,Kept from the last shut down: 0\.00 % \(count 0\)\r?\n/.test(events(s, 5)), events(s, 5));
    s.turnOn();
    s.cmd('VALVE SET 20', 60000);
    check('start after Shut down: on, moves at once', s.cmd('MODE?;VALVE?') === 'MODE MANUAL;VALVE 20.00 %\r\n' &&
      s.get(5) / 2 === 400 + Math.round(66048 * 0.2), s.cmd('MODE?;VALVE?') + ' needle ' + s.get(5));
  }
}
{ // The valve closed onto the switch while the power was off: not homed, nothing offered.
  const s = load(); s.boot(parkedFlash, { off: true }); s.set(5, 0); s.run(2000);
  check('kept but switch pressed: not homed', s.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE NONE\r\n' && s.offer() === -2147483648,
    s.cmd('MODE?;VALVE?') + ' offer ' + s.offer());
  check('kept but switch pressed: event', /limit switch is pressed: home the valve/.test(events(s, 5)), events(s, 5));
  s.turnOn();
  check('kept but switch pressed: on, NOT HOMED', s.cmd('MODE?') === 'MODE NOT HOMED\r\n', s.cmd('MODE?'));
  const t = load(); t.boot(s.flash(), { off: true }); t.set(5, NEEDLE0); t.run(2000);
  check('kept entry withdrawn', t.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE NONE\r\n' && t.offer() === -2147483648,
    t.cmd('MODE?;VALVE?') + ' offer ' + t.offer());
}
{ // Set by hand, not homed: kept through Shut down, but still not homed (SAVED).
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.setClosed();
  check('set by hand at 0 %: OFF at once', s.shutdown(300) && s.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE 0.00 %\r\n', s.why() + s.cmd('MODE?;VALVE?'));
  const t = load(); t.boot(s.flash(), { off: true }); t.set(5, 40000); t.run(2000);
  check('set by hand: kept, not homed', t.cmd('VALVE?') === 'VALVE 0.00 %\r\n' &&
    /Kept from the last shut down: 0\.00 % \(count 0\), not homed/.test(events(t, 5)), t.cmd('VALVE?') + events(t, 5));
}
{ // Switched off without Shut down: the saved position offered, and the log says so.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000); s.run(3000);
  const t = load(); t.boot(s.flash()); t.set(5, 40000); t.run(2000);
  check('switched off without Shut down', t.offer() === Math.round(66048 * 0.3) && /Last power-off without Shut down/.test(events(t, 5)),
    t.offer() + '\n' + events(t, 5));
  shot(t, 'unparked-ask');
}
{ // Power lost in AUTO: the position log keeps the mode and setpoint, the start-up says so.
  const s = homed(load());
  s.cmd('SETPOINT 8'); s.cmd('MODE AUTO', 20000);
  const t = load(); t.boot(s.flash()); t.set(5, 40000); t.run(2000);
  check('power lost in AUTO: the start-up question and the log name AUTO and the setpoint', t.offer() !== -2147483648 &&
    /without Shut down, in AUTO at 8\.0 mbar/.test(events(t, 5)), events(t, 5));
  shot(t, 'power-lost-auto');
}
{ // The power icon, time-outs, STOP and the switch at the seat.
  const s = homed(load());
  const screen = () => { const m = new Uint8Array(s.x.memory.buffer); let p = s.x.sim_screen(), e = p; while (m[e]) e++;
    return Buffer.from(m.subarray(p, e)).toString(); };
  s.tap(415, 184); s.tap(22, 20, 100); s.tap(22, 20, 300);          // Menu, then Home and a quick second tap there
  const guard = !s.popup() && screen() === 'Home';
  s.run(1200); s.tap(22, 20); const asks = s.popup(); s.run(1000); s.tap(100, 240);   // past the question's 1 s guard
  check('the power icon ignores taps for 1 s after Home opens', guard && asks, [guard, asks].join());
  s.tap(290, 184); const kp = s.popup(); s.run(61000);
  check('a keypad closes after 60 s untouched', kp && !s.popup(), [kp, s.popup()].join());
  s.tap(415, 184); s.tap(240, 72); const pg = screen(); s.run(601000);
  check('a page goes back to Home after 10 min untouched', pg === 'Events' && screen() === 'Home', pg + ' ' + screen());
  s.tap(240, 20);                                                    // brightens the dimmed screen only
  s.tap(415, 184); s.tap(69, 72); s.run(601000);
  check('Trend stays', screen() === 'Trend', screen());
  s.tap(22, 20); s.run(1200);
  s.cmd('SETPOINT 5'); s.cmd('MODE AUTO', 3000);
  s.tap(22, 20); const pa = s.popup();
  check('Power options refused in AUTO', !pa && s.cmd('MODE?') === 'MODE AUTO\r\n', pa + s.cmd('MODE?'));
  s.cmd('MODE MANUAL', 3000);
  // Closing onto the switch where homing found it: no question.
  s.cmd('VALVE SET 0', 30000);
  const c = load(); c.boot(); c.set(5, 40000); c.run(2000); c.home();
  c.cmd('VALVE SET 0', 20000);
  c.tap(415, 184); c.tap(82, 128, 400);                               // Menu, Motor (Jog)
  c.x.sim_touch(64, 156, 1); c.run(6000); c.x.sim_touch(64, 156, 0); c.run(1000);   // hold Close onto the switch
  const ev = events(c, 6);
  check('Close onto the homed switch point: no question, "Closed onto the switch"', !c.popup() &&
    /Closed onto the limit switch at its homed point/.test(ev) && !/W-1,Limit switch pressed/.test(ev), c.popup() + ' ' + ev);
  shot(c, 'on-the-switch');
}
{ // STOP cancels: MANUAL, the valve where it stopped.
  const s = homed(load());
  s.cmd('VALVE SET 50', 90000); s.run(2000);
  s.shutdown(5000);
  s.cmd('STOP', 1000);
  const v = parseFloat(s.cmd('VALVE?').match(/VALVE ([-\d.]+)/)[1]);
  check('STOP cancels shut down: MANUAL', s.cmd('MODE?') === 'MODE MANUAL\r\n' && v > 40 && v < 48 && !s.busy(), s.cmd('MODE?;VALVE?'));
  check('STOP cancels shut down: event', /shut down cancelled/.test(events(s, 5)), events(s, 5));
}
{ // From AUTO: AUTO ends, then the same move to 0 %.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000);
  s.cmd('SETPOINT 3;MODE AUTO', 20000);
  check('in AUTO before shut down', s.cmd('MODE?') === 'MODE AUTO\r\n', s.cmd('MODE?'));
  check('shut down from AUTO', s.shutdown(1000) && s.cmd('MODE?') === 'MODE SHUTDOWN\r\n', s.why() + s.cmd('MODE?'));
  s.run(90000);
  check('shut down from AUTO: OFF at 0 %', s.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE 0.00 %\r\n', s.cmd('MODE?;VALVE?'));
}
{ // Not homed: OFF at once, the valve not moved.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  check('shut down not homed: OFF, not moved', s.shutdown(2000) && s.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE NONE\r\n' &&
    s.get(5) === 40000, s.why() + s.cmd('MODE?;VALVE?') + ' needle ' + s.get(5));
  check('shut down not homed: event', /OFF,Shut down, not homed: valve not moved/.test(events(s, 5)), events(s, 5));
}
{ // The switch before 0 % (ice, an early switch): MANUAL there and the switch question.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000);
  s.set(18, 4000); s.set(7, 3);   // trips 2000 full steps above the seat
  s.shutdown(60000);
  const st = s.cmd('MODE?;VALVE?');
  check('early switch: MANUAL, count kept', /^MODE MANUAL;VALVE 2\.\d\d %/.test(st), st);
  check('early switch: event and question', /Shut down stopped \(limit switch pressed\)/.test(events(s, 5)) &&
    /W-1,Limit switch pressed/.test(events(s, 5)), events(s, 5));
  shot(s, 'early-switch');
  // The switch question needs an answer: a tap round it or on the strip leaves it.
  const asked = s.popup();
  s.tap(455, 100); s.tap(100, 240);
  check('switch question stays on a tap round it', asked && s.popup() && /W-1/.test(s.cmd('ALARM?')), [asked, s.popup(), s.cmd('ALARM?')].join());
}
{ // Already at 0 %: OFF at once.
  const s = homed(load());
  check('at 0 %: OFF at once', s.shutdown(300) && s.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE 0.00 %\r\n', s.why() + s.cmd('MODE?;VALVE?'));
}
{ // PIDS SET on zone 1: the PID tab's Save over the text port, also in AUTO.
  const s = homed(load());
  const one = c => s.cmd(c, 1000);
  const set1 = () => one('PIDS?').trim().split(',').slice(1, 4).join();
  check('PIDS? zone 1 defaults', set1() === '0.06,0.60,0.00', one('PIDS?'));
  check('PIDS SET kp,ti silent, PIDS? confirms', one('PIDS SET 1,3.5,0.5') === '' && set1() === '3.50,0.50,0.00', one('PIDS?'));
  check('PIDS SET kp,ti,kd', one('PIDS SET 1, 3.5, 0.5, 1.25') === '' && set1() === '3.50,0.50,1.25', one('PIDS?'));
  check('PIDS SET saves the settings', s.setting(CFG.KP) === 350 && s.setting(CFG.TI) === 50 && s.setting(CFG.KD) === 125,
    [s.setting(CFG.KP), s.setting(CFG.TI), s.setting(CFG.KD)].join());
  const bad = [['PIDS SET 1', 101], ['PIDS SET 1,3', 101], ['PIDS SET 1,3,0.5,1,2', 101], ['PIDS SET 1,x,1', 101], ['PIDS SET 1,3,,1', 101],
    ['PIDS SET 1,0,0.5', 102], ['PIDS SET 1,101,0.5', 102], ['PIDS SET 1,3,-1', 102], ['PIDS SET 1,3,0.5,101', 102]];
  const wrong = bad.map(([c, n]) => [c, n, one(c)]).filter(([c, n, r]) => !new RegExp('^ERR ' + n + ' [A-Z][^;\\r\\n]*\\r\\n$').test(r))
    .map(([c, n, r]) => c + ' -> ' + JSON.stringify(r) + ' (want ' + n + ')');
  check('PIDS SET zone 1 refusals', wrong.length === 0, wrong.join('\n     '));
  check('PIDS SET range text', one('PIDS SET 1,0,0.5') === 'ERR 102 Outside 0.01 to 100.00 for KP\r\n', one('PIDS SET 1,0,0.5'));
  check('refused PIDS SET changes nothing', set1() === '3.50,0.50,1.25', one('PIDS?'));
  s.cmd('VALVE SET 30', 60000);
  s.cmd('SETPOINT 3;MODE AUTO', 20000);
  check('PIDS SET in AUTO, AUTO kept', one('PIDS SET 1,2,0.4;MODE?') === 'MODE AUTO\r\n' && set1() === '2.00,0.40,1.25', one('PIDS?;MODE?'));
  check('PIDS SET in AUTO saves', s.setting(CFG.KP) === 200 && s.setting(CFG.TI) === 40, s.setting(CFG.KP) + ' ' + s.setting(CFG.TI));
}
{ // PID table: PIDS?, PIDS SET, TABLE?, TABLE SET, and AUTO switching zones on the pressure with hysteresis.
  const s = homed(load());
  const one = c => s.cmd(c, 1000);
  check('PIDS? defaults: the bench table', one('PIDS?') === 'PIDS 1,0.06,0.60,0.00,0.12,0.82,0.00,0.80,0.94,0.00,0.40,0.25,0.00\r\n',
    one('PIDS?'));
  check('TABLE? defaults: on the pressure, 6, 10 and 14 mbar, 15 %', one('TABLE?') === 'TABLE 6.0,10.0,14.0,15\r\n', one('TABLE?'));
  check('PIDS SET silent, PIDS? confirms', one('PIDS SET 2,5,0.3') === '' && one('PIDS SET 3, 6, 0.35, 0.5') === '' &&
    one('PIDS SET 4,7,0.4') === '' &&
    one('PIDS?') === 'PIDS 1,0.06,0.60,0.00,5.00,0.30,0.00,6.00,0.35,0.50,7.00,0.40,0.00\r\n', one('PIDS?'));
  check('PIDS SET saves the settings', s.setting(CFG.KP2) === 500 && s.setting(CFG.TI2) === 30 && s.setting(CFG.KD3) === 50 &&
    s.setting(CFG.KP4) === 700 && s.setting(CFG.TI4) === 40, [s.setting(CFG.KP2), s.setting(CFG.TI2), s.setting(CFG.KD3),
    s.setting(CFG.KP4), s.setting(CFG.TI4)].join());
  const bad = [['PIDS SET', 101], ['PIDS SET 2', 101], ['PIDS SET 2,1', 101], ['PIDS SET x,1,1', 101], ['PIDS SET 2,1,1,1,1', 101],
    ['PIDS SET 0,1,1', 102], ['PIDS SET 5,1,1', 102], ['PIDS SET 2,0,1', 102], ['PIDS SET 3,1,-1', 102],
    ['TABLE SET', 101], ['TABLE SET ON,6,10,14,15', 101], ['TABLE SET 6,10,14', 101], ['TABLE SET 6,20,15', 101],
    ['TABLE SET 6,10,14,15,1', 101], ['TABLE SET x,10,14,15', 101], ['TABLE SET 0,10,14,15', 102],
    ['TABLE SET 6,10,14,41', 102], ['TABLE SET 20,6,30,15', 102], ['TABLE SET 10,11,20,15', 102],
    ['TABLE SET 6,10,11,15', 102], ['TABLE SET 6,10,-1,15', 102]];
  const wrong = bad.map(([c, n]) => [c, n, one(c)]).filter(([c, n, r]) => !new RegExp('^ERR ' + n + ' [A-Z][^;\\r\\n]*\\r\\n$').test(r))
    .map(([c, n, r]) => c + ' -> ' + JSON.stringify(r) + ' (want ' + n + ')');
  check('PIDS SET and TABLE SET refusals', wrong.length === 0, wrong.join('\n     '));
  check('TABLE SET boundaries text', one('TABLE SET 20,6,30,15') === 'ERR 102 B2 must be above B1, by more than the hysteresis either side\r\n' &&
    one('TABLE SET 6,10,11,15') === 'ERR 102 B3 must be above B2, by more than the hysteresis either side, or 0 for three zones\r\n',
    one('TABLE SET 20,6,30,15') + one('TABLE SET 6,10,11,15'));
  check('refusals change nothing', one('TABLE?;PIDS?') ===
    'TABLE 6.0,10.0,14.0,15;PIDS 1,0.06,0.60,0.00,5.00,0.30,0.00,6.00,0.35,0.50,7.00,0.40,0.00\r\n', one('TABLE?;PIDS?'));
  check('TABLE SET, B3 0: three zones', one('TABLE SET 6,20,0,15') === '' && one('TABLE?') === 'TABLE 6.0,20.0,0.0,15\r\n' &&
    s.setting(CFG.B1) === 60 && s.setting(CFG.B2) === 200 && s.setting(CFG.B3) === 0, one('TABLE?'));
  // The sim plant runs about 0.8 to 12.5 mbar (20 to 100 % open), so the
  // boundaries here are 3, 6 and 8 mbar with 10 %: up at 3.3, 6.6 and 8.8,
  // down at 7.2, 5.4 and 2.7. Sim-plant gains, a different set each.
  one('TABLE SET 3,6,8,10;PIDS SET 1,6,0.25;PIDS SET 2,5,0.25,0');
  one('PIDS SET 3,4.5,0.25,0;PIDS SET 4,4,0.25,0');
  // The set changes so far, oldest first (EVENT? lists the newest first).
  const sws = () => [...events(s, 100).matchAll(/AUTO PID zone (\d) to (\d), pressure ([\d.]+) mbar/g)].map(m => [+m[1], +m[2], +m[3]]).reverse();
  s.cmd('VALVE SET 40', 60000);
  s.run(240000);
  s.cmd('SETPOINT 10;MODE AUTO', 1000);
  check('AUTO starts on the zone for the lower of pressure and setpoint (3.8 mbar: zone 2)', /AUTO at setpoint 10\.0 mbar, valve [\d.]+ %, PID zone 2/.test(events(s, 3)),
    events(s, 3));
  s.run(600000);
  check('PID table: zone 4 at 10 mbar', /^PIDS 4,/.test(one('PIDS?')), one('PRESSURE?;PIDS?'));
  const first = sws();
  check('then 2 to 3 over 6.6 and 3 to 4 over 8.8 mbar (B x 1.1), once each', first.length === 2 && first[0][0] === 2 &&
    first[0][1] === 3 && first[0][2] >= 6.6 && first[0][2] < 7.5 && first[1][0] === 3 && first[1][1] === 4 && first[1][2] >= 8.8 &&
    first[1][2] < 9.7, JSON.stringify(first));
  // PIDS SET on the set in use, in AUTO.
  check('PIDS SET edits the set in use', one('PIDS SET 4,3.8,0.25') === '' && /^PIDS 4,.*,3\.80,0\.25,0\.00\r\n$/.test(one('PIDS?')) && s.setting(CFG.KP4) === 380 &&
    s.setting(CFG.KP3) === 450 && s.setting(CFG.KP) === 600, [s.setting(CFG.KP4), s.setting(CFG.KP3), s.setting(CFG.KP)].join());
  s.cmd('SETPOINT 2', 1000);
  s.run(900000);
  // Going down the zone follows the setpoint at once (the lower of
  // pressure and setpoint), so a big step down does not start on zone 4.
  const sw = sws().slice(2);
  check('10 -> 2: zone 4 to 1 at once, still at 10 mbar (the setpoint\'s zone), no other change', sw.length === 1 && sw[0][0] === 4 &&
    sw[0][1] === 1 && sw[0][2] > 9, JSON.stringify(sw));
  check('10 -> 2: AUTO kept, near 2 mbar, zone 1', one('MODE?') === 'MODE AUTO\r\n' && Math.abs(parseFloat(one('PRESSURE?').slice(9)) - 2) < 0.3 &&
    /^PIDS 1,6\.00,0\.25,0\.00,/.test(one('PIDS?')), one('MODE?;PRESSURE?;PIDS?'));
  s.cmd('SETPOINT 10', 1000);
  s.run(900000);
  const up = sws().slice(3);
  check('2 -> 10: zone 1 to 2 over 3.3, 2 to 3 over 6.6, 3 to 4 over 8.8 mbar', up.length === 3 && up[0][0] === 1 && up[0][1] === 2 &&
    up[0][2] >= 3.3 && up[0][2] < 4.3 && up[1][0] === 2 && up[1][1] === 3 && up[1][2] >= 6.6 && up[1][2] < 7.6 &&
    up[2][0] === 3 && up[2][1] === 4 && up[2][2] >= 8.8 && up[2][2] < 9.8, JSON.stringify(up));
  shot(s, 'pid-table');
  check('TABLE SET with a word before the boundaries refused', one('TABLE SET PV,6,10,14,15') === 'ERR 101 Expected: TABLE SET <b1>,<b2>,<b3>,<h>\r\n' &&
    one('TABLE?') === 'TABLE 3.0,6.0,8.0,10\r\n', one('TABLE SET PV,6,10,14,15') + one('TABLE?'));
}
{ // PID > Zones: the Zone n labels keep their place as the zone in use changes.
  const s = load(); s.boot(); s.run(2000); s.set(40, 10); s.run(5000);
  s.tap(415, 71, 600); s.tap(180, 71, 600); s.tap(179, 71, 600);
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const where = () => { s.x.sim_textfit_all(1); const t = str(s.x.sim_textfit()); s.x.sim_textfit_all(0);
    return t.split('\n').filter(l => /"Zone \d"/.test(l)).map(l => (l.match(/"Zone \d".*? at \d+,\d+/) || [l])[0]).join(' | '); };
  const layouts = new Set(), zones = new Set();
  for (let k = 0; k < 20; k++) {
    s.set(40, k % 2 ? 30 : 2);
    for (let i = 0; i < 6; i++) { s.run(50); layouts.add(where()); zones.add(s.cmd('PIDS?').slice(5, 6)); }
  }
  check('PID > Zones: the Zone 1 to 4 labels stay put while the zone in use moves through all four', zones.size === 4 &&
    layouts.size === 1 && /Zone 4/.test([...layouts][0]), [...zones].join() + '\n     ' + [...layouts].join('\n     '));
}
{ // PID > Zones keypads: 0 for Zone 4 from gives None, three zones; a refusal gives the limit and what sets it; the
  // first figure after a refusal starts a new entry (0 after "5" refused is 0, not 50).
  const s = homed(load());
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const pop = () => str(s.x.sim_popup_text()).replace(/\n/g, ' ');
  const labels = () => { s.x.sim_textfit_all(1); const t = str(s.x.sim_textfit()); s.x.sim_textfit_all(0); return t; };
  const K = { 7: [46, 134], 8: [108, 134], 9: [170, 134], 4: [46, 184], 5: [108, 184], 6: [170, 184], 0: [232, 184], 1: [46, 234], 2: [108, 234], 3: [170, 234],
    '.': [232, 234], S: [305, 208], C: [305, 134] };   // the keypad's keys, Set and Cancel
  const type = t => { for (const c of t) s.tap(...K[c], 200); };
  const Z2 = [64, 183], Z3 = [181, 183], Z4 = [298, 183], HYST = [100, 183];
  s.tap(415, 184); s.tap(396, 72); s.tap(180, 71, 600);   // Menu > PID > Zones
  s.tap(...Z4); type('5S'); const low4 = pop();
  type('0'); const entry = /"0" \| text \d+x27, box 125x27/.test(labels()), open = s.popup();
  type('S');
  check('Zone 4 from: 5 refused with the limit; then 0, Set gives None, three zones', low4 ===
    '0 for no Zone 4, or 13.6 mB or more: Zone 3 starts at 10.0 mB (15 % hysteresis).' && entry && open && !s.popup() &&
    s.setting(CFG.B3) === 0 && /"None"/.test(labels()) && !/"Zone 4" \| text[^\n]* at \d+,10\d/.test(labels()) &&
    s.cmd('TABLE?') === 'TABLE 6.0,10.0,0.0,15\r\n', [low4, entry, open, s.popup(), s.setting(CFG.B3), s.cmd('TABLE?')].join(' | '));
  s.tap(...Z4); type('0S'); const none = !s.popup() && s.setting(CFG.B3) === 0;
  s.tap(...Z4); type('13.5S'); const r135 = s.popup(); type('13.6S');
  check('Zone 4 from: 0 on None stays None; 13.5 refused, 13.6 set', none && r135 && !s.popup() && s.setting(CFG.B3) === 136,
    [none, r135, s.popup(), s.setting(CFG.B3)].join());
  s.tap(...Z4); type('14S');
  s.tap(...Z2); type('8S'); const hi2 = pop(); s.tap(...K.C);
  s.tap(...Z3); type('5S'); const lo3 = pop(); type('12S'); const hi3 = pop(); type('0S'); const zero3 = pop(); s.tap(...K.C);
  check('Zone 2 from and Zone 3 from refusals: the limit and the zone that sets it; 0 is out of range there',
    hi2 === 'Must be 7.3 mB or less: Zone 3 starts at 10.0 mB (15 % hysteresis).' &&
    lo3 === 'Must be 8.2 mB or more: Zone 2 starts at 6.0 mB (15 % hysteresis).' &&
    hi3 === 'Must be 10.3 mB or less: Zone 4 starts at 14.0 mB (15 % hysteresis).' && zero3 === 'Out of range: 0.1 to 1000.0' &&
    s.cmd('TABLE?') === 'TABLE 6.0,10.0,14.0,15\r\n', [hi2, lo3, hi3, zero3, s.cmd('TABLE?')].join(' | '));
  s.tap(291, 71, 600); s.tap(...HYST); type('25S'); const wide = pop(); type('16S');   // Settings > Hysteresis
  check('Hysteresis: 25 refused with the most that fits, 16 set', wide === 'Must be 16 % or less: above that the zones overlap.' &&
    !s.popup() && s.setting(CFG.HYST) === 16, [wide, s.popup(), s.setting(CFG.HYST)].join(' | '));
  // Three zones with Zone 3 too high for a fourth; then a tap on the greyed Zone 4 (Gains).
  s.tap(...HYST); type('15S'); s.tap(180, 71, 600); s.tap(...Z4); type('0S'); s.tap(...Z3); type('900S');
  s.tap(...Z4); type('950S'); const nofit = pop(); s.tap(...K.C);
  s.tap(69, 71, 600); s.tap(304, 127, 1000); const grey = labels();
  check('Zone 4 from with no room above Zone 3; the greyed Zone 4 says where to set it',
    nofit === 'No Zone 4 fits: Zone 3 starts at 900.0 mB (15 % hysteresis). Enter 0.' &&
    /"Zone 4 is not used\. Set/.test(grey), nofit + ' | ' + (grey.match(/"Zone 4 is[^"]*"/) || [grey])[0]);
}
{ // A PID gain is taken while the valve moves in MANUAL, as it is in AUTO.
  const s = homed(load()); s.cmd('VALVE SET 90', 500);
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  s.tap(415, 184); s.tap(396, 72); s.tap(64, 183); s.tap(108, 184, 200); s.tap(305, 208, 600);   // Menu > PID > KP, 5, Set
  check('PID keypad while the valve moves in MANUAL: the gain is set', s.busy() && !s.popup() &&
    s.setting(CFG.KP) === 500, str(s.x.sim_popup_text()) + ' ' + s.setting(CFG.KP));
}
{ // An event keeps its second: the plain sum (the clock's whole second less the age in whole seconds) flips by 1
  // for part of every second, ui_when does not. The plain sum and ui_when side by side.
  const s = load(); s.boot(); s.run(2000); s.run(1234); const t0 = s.x.sim_ms();
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  const hms = v => [Math.floor(v / 3600), Math.floor(v / 60) % 60, v % 60].map(x => String(x).padStart(2, '0')).join(':');
  const now = new Set(), old = new Set();
  for (let i = 0; i < 300; i++) {
    s.run(37);
    now.add(str(s.x.sim_when(s.x.sim_ms() - t0)));
    const c = s.cmd('TIME?').trim().slice(-8).split(':').map(Number);
    old.add(hms(c[0] * 3600 + c[1] * 60 + c[2] - Math.floor((s.x.sim_ms() - t0) / 1000)));
  }
  check('an event\'s time keeps its second over 11 s of redraws (the plain sum flips between two)', now.size === 1 && old.size === 2,
    [...now].join() + ' / old ' + [...old].join());
}
{ // Past the 49.7 day tick wrap: lv_tick_get wraps at 2^32 ms, so an event, a latched fault or the last calibration
  // older than that would read as recent (its age modulo 49.7 days).
  // The tick moved on 50 days in two steps, the clock not: the event reads 50 days back at the same time of day, EVENT?
  // gives its true age, a fault latched before the wrap keeps its time, and Up time counts on.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
  s.set(8, 0); s.run(3000);                                   // F-1: the Druck unplugged
  s.cmd('SETPOINT 5', 500); s.run(500);
  const t0 = str(s.x.sim_event_when(0)), f0 = str(s.x.sim_event_when(2));   // the setpoint, F-1
  for (let k = 0; k < 2; k++) { s.x.sim_tick_add(25 * 86400000); s.run(2000); }
  const t1 = str(s.x.sim_event_when(0)), f1 = str(s.x.sim_event_when(2));
  const age = parseFloat(events(s, 1).split('\n').find(l => /setpoint 5/.test(l)) || 'NaN');
  s.tap(240, 20);                                              // brightens the dimmed screen only
  s.tap(415, 184); s.tap(84, 184);                             // Menu > System: its Up time card
  s.x.sim_textfit_all(1); const up = (/"(\d+ d \d+ h)"/.exec(str(s.x.sim_textfit())) || [])[1]; s.x.sim_textfit_all(0);
  check('past the 49.7 day tick wrap: an event and a fault read 50 days back at their time, EVENT? gives the true age, Up time 50 d',
    /^\d\d:\d\d:\d\d$/.test(t0) && t1 === '04/09/26 ' + t0.slice(0, 5) && f1 === '04/09/26 ' + f0.slice(0, 5) && age > 50 * 86400 && age < 50 * 86400 + 10 && up === '50 d 0 h',
    [t0, t1, f0, f1, age, up].join(' | '));
}
{ // The limit switch pressed away from the seat: a yellow edge round Home's Valve tile at once and, within 0.3 s,
  // W-1 and its question; released, the edge goes and the question waits for its answer.
  const s = homed(load()); s.cmd('VALVE SET 20', 60000);
  const px = (x, y) => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[y * 480 + x];
  const before = px(187, 100);
  s.set(7, 1); s.run(100);   // the wire open: the NC contact reads pressed
  const edge = px(187, 100);  // before the question covers the tile
  shot(s, 'limit-edge');
  s.run(100);
  const al = s.cmd('ALARM?', 100);
  check('limit pressed with the valve still: yellow edge on the Valve tile, W-1 within 0.3 s', before === 0xFFFF && edge === 0xF606 &&
    al === 'ALARM W-1,0,Limit-switch question waiting at the screen\r\n', [before.toString(16), edge.toString(16), al.trim()].join(' '));
  s.run(1000);
  const ev = events(s, 5), asked = s.popup();
  shot(s, 'w01-still');
  check('W-1 with the valve still: one event, the question on the screen, no W-4', asked &&
    (ev.match(/W-1,Limit switch pressed at [\d.]+ % \(count/g) || []).length === 1 && !/W-4/.test(ev), asked + ' ' + ev);
  s.tap(160, 147); s.run(3000);   // Skip, the switch still pressed: not asked again
  const skipped = s.cmd('ALARM?'), ev2 = events(s, 3);
  check('W-1 Skip: count kept, not asked again while the switch stays pressed', skipped === 'ALARM NONE\r\n' && !s.popup() &&
    /W-1,Skipped: position kept/.test(ev2) && /^VALVE 20\.00 %/.test(s.cmd('VALVE?')), skipped + ev2 + s.cmd('VALVE?'));
  s.set(7, 0); s.run(300);
  const after = px(187, 100);
  s.set(7, 1); s.run(1000);       // released and pressed again: a new press, asked again
  check('W-1: the edge goes when released; a new press asks again', after === 0xFFFF && s.popup() && /W-1/.test(s.cmd('ALARM?')),
    [after.toString(16), s.popup(), s.cmd('ALARM?').trim()].join(' '));
}
{ // W-1 in AUTO: the switch pressed away from the seat ends AUTO, MANUAL with the valve
  // idle, and the question waits. Start AUTO is refused while it waits.
  const s = oldGains(homed(load()));
  s.cmd('SETPOINT 5'); s.cmd('MODE AUTO'); s.run(300000);
  const at = s.cmd('VALVE?');
  s.set(7, 1); s.run(2000);
  const ev = events(s, 5), st = s.cmd('MODE?;ALARM?');
  check('W-1 in AUTO, away from the seat: MANUAL, the question, one W-1 event', /MODE,MANUAL after W-1/.test(ev) &&
    (ev.match(/W-1,Limit switch pressed at [\d.]+ % \(count/g) || []).length === 1 &&
    st === 'MODE MANUAL;ALARM W-1,0,Limit-switch question waiting at the screen\r\n' && s.popup(), at.trim() + ' | ' + ev + st);
  const refused = s.cmd('MODE AUTO');
  check('W-1: Start AUTO refused while the switch is pressed away from the seat', /^ERR \d+ /.test(refused) && s.cmd('MODE?') === 'MODE MANUAL\r\n', refused);
}
{ // A closing move that reaches the switch away from the seat raises W-1 once.
  const s = homed(load()); s.cmd('VALVE SET 30', 60000);
  s.set(18, 12000); s.set(7, 3);   // trips 6000 full steps above the seat
  s.cmd('VALVE SET 2', 90000); s.run(2000);
  const ev = events(s, 6), st = s.cmd('MODE?;ALARM?');
  check('closing onto an early switch: one W-1, the question, no second warning', (ev.match(/W-1,Limit switch pressed at [\d.]+ % \(count/g) || []).length === 1 &&
    !/W-4/.test(ev) && st === 'MODE MANUAL;ALARM W-1,0,Limit-switch question waiting at the screen\r\n', ev + st);
  s.tap(318, 155); s.run(5000);    // Set as closed: the switch point, then up to 0 %
  const evc = events(s, 6);
  check('W-1 Set as closed: the count becomes the switch point, then 0 %, nothing left showing', /W-1,Point set as closed/.test(evc) &&
    s.cmd('ALARM?') === 'ALARM NONE\r\n' && s.cmd('VALVE?') === 'VALVE 0.00 %\r\n', evc + s.cmd('ALARM?') + s.cmd('VALVE?'));
}
{ // Disable switch, the pass band (the force switch can trip early where the valve binds): a toggle on Valve >
  // Limit; between From and To, 5 and 50 %, the pressed switch neither stops closing nor raises W-1; below From it
  // stops.
  const s = homed(load()); s.cmd('VALVE SET 40', 150000);
  s.tap(415, 184); s.tap(240, 128); s.tap(291, 72); s.tap(273, 184);   // Menu > Valve > Limit, Disable switch on
  shot(s, 'valve-ignore');
  const ev1 = events(s, 1), on1 = /Disable switch set to On, 5 to 50 % \(screen\)/.test(ev1);
  s.tap(273, 184); const off = /Disable switch set to Off \(screen\)/.test(events(s, 1));   // a second press: off
  s.tap(273, 184);                                                                                    // and on again
  const on = on1 && off && /Disable switch set to On, 5 to 50 % \(screen\)/.test(events(s, 1));
  check('Disable switch: on, off and on again from the toggle', on, ev1.trim());
  { // The disabled range in Home's yellow (0xF2C230, RGB565 0xF606), off grey. "Switch released" for the switch not
    // pressed, on the strip and on Diagnostics.
    const fb = () => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272);
    const yellow = () => { const f = fb(); let n = 0; for (let y = 104; y < 152; y++) for (let x = 10; x < 353; x++) n += f[y * 480 + x] === 0xF606; return n; };
    const str = p => { const m = new Uint8Array(s.x.memory.buffer); let e = p; while (m[e]) e++; return Buffer.from(m.subarray(p, e)).toString(); };
    const dump = () => { s.x.sim_textfit_all(1); const d = str(s.x.sim_textfit()); s.x.sim_textfit_all(0); return d; };
    s.run(600); const yOn = yellow(), strip = /"Switch released\. Disabled 5 to 50 %"/.test(dump());
    s.tap(273, 184); s.run(600); const yOff = yellow(); s.tap(273, 184); s.run(600);   // off, then on again
    s.tap(66, 20); s.tap(396, 184); s.run(600); const diag = /"Released"/.test(dump()) && !/"Open"/.test(dump());
    s.tap(22, 20);
    check('Valve > Limit: the disabled range in Home\'s yellow while on, none while off; "Switch released" on the strip and Diagnostics',
      yOn > 100 && yOff === 0 && strip && diag, [yOn, yOff, strip, diag].join());
    shot(s, 'valve-limit-yellow');
  }
  s.set(7, 1); s.run(1000);                            // the switch reads pressed at 40 %
  const a1 = s.cmd('ALARM?');
  s.cmd('VALVE SET 6', 90000);
  const at6 = s.cmd('VALVE?'), a6 = s.cmd('ALARM?');
  s.cmd('VALVE SET 2', 30000);
  const at5 = s.cmd('VALVE?'), ev = events(s, 5);
  check('pass band: no W-1 at 40 %, closes to 6 % past the pressed switch, stops at 5 %', on && a1 === 'ALARM NONE\r\n' &&
    /VALVE 6\.0/.test(at6) && a6 === 'ALARM NONE\r\n' && /VALVE (4\.9|5\.0)/.test(at5) && /W-1/.test(ev),
    [on, a1.trim(), at6.trim(), a6.trim(), at5.trim(), ev.slice(-300)].join(' | '));
}
{ // The Disable switch edges are saved settings: From 10 from
  // its card's keypad and Save, and closing past the pressed switch then stops at 10 %. From at or above
  // To is refused and nothing is saved.
  const s = homed(load()); s.cmd('VALVE SET 40', 150000);
  s.tap(415, 184); s.tap(240, 128); s.tap(291, 72); s.tap(273, 184);   // Menu > Valve > Limit, Disable switch on
  s.tap(52, 184); s.tap(40, 239); s.tap(230, 183); s.tap(320, 212);   // From: 1 0, Set
  shot(s, 'valve-ignore-from');
  s.tap(415, 128);                                                     // Save
  const saved = s.setting(CFG.PASS_LO);
  s.set(7, 1); s.run(1000);                                            // the switch reads pressed at 40 %
  s.cmd('VALVE SET 2', 120000);
  const at = s.cmd('VALVE?'), ev = events(s, 5);
  s.set(7, 0); s.run(1000);
  s.tap(415, 184); s.tap(240, 128); s.tap(291, 72);
  s.tap(52, 184); s.tap(170, 183); s.tap(230, 183); s.tap(320, 212);  // From: 6 0, above To 50
  s.tap(415, 128);
  const kept = s.setting(CFG.PASS_LO);
  check('Disable switch: From saved as 10 stops closing at 10 %; From over To refused', saved === 10 &&
    /VALVE (9\.9|10\.0)/.test(at) && /W-1/.test(ev) && kept === 10,
    [saved, at.trim(), kept, ev.slice(-200)].join(' | '));
}
{ // Backlight: 10 % after 10 min with no touch, the first touch only
  // brightens it (STOP still stops), a network set or the limit switch question brightens it; off while OFF
  // 30 s after the last touch, a tap lights it and counts as the first of the double tap.
  const bl = s => s.x.sim_backlight();
  const scr = s => { const m = new Uint8Array(s.x.memory.buffer); let p = s.x.sim_screen(), e = p; while (m[e]) e++;
    return Buffer.from(m.subarray(p, e)).toString(); };
  const touch = (s, x, y) => { s.x.sim_touch(x, y, 1); s.run(60); s.x.sim_touch(x, y, 0); s.run(100); };
  const o = load(); o.boot(undefined, { off: true }); o.set(5, 40000);
  o.run(1000); const off1 = bl(o); o.run(30000); const off31 = bl(o);
  touch(o, 240, 136); const offTap = bl(o), stillOff = /^MODE OFF/.test(o.cmd('MODE?'));
  touch(o, 240, 136); o.run(3000); const on = !/^MODE (OFF|SHUTDOWN)/.test(o.cmd('MODE?'));
  check('backlight while OFF: full, off 30 s after power-up, a tap lights it and is the first of the double tap',
    off1 === 255 && off31 === 0 && offTap === 255 && stillOff && on, [off1, off31, offTap, stillOff, on].join());
  const s = homed(load());
  s.cmd('VALVE SET 40', 60000);
  s.run(9 * 60000); const at9 = bl(s); s.run(70000); const at10 = bl(s);
  s.tap(415, 184); const woke = bl(s), stayed = scr(s); s.tap(415, 184); const menu = scr(s);
  check('backlight: 10 % after 10 min with no touch; the first touch brightens it and opens nothing',
    at9 === 255 && at10 === 26 && woke === 255 && stayed === 'Home' && menu === 'Menu', [at9, at10, woke, stayed, menu].join());
  s.tap(22, 20); s.run(11 * 60000); const dim1 = bl(s);
  s.tap(415, 240); const stopEv = events(s, 3), stopOk = bl(s) === 255 && /STOP/.test(stopEv);
  s.run(11 * 60000); const dim2 = bl(s); s.cmd('SETPOINT 5'); s.run(600); const net = bl(s);
  s.run(11 * 60000); const dim3 = bl(s); s.set(7, 1); s.run(1500); const q = bl(s), asked = !!s.popup();
  check('backlight: STOP stops on the dimmed screen; a network set and the limit switch question brighten it',
    dim1 === 26 && stopOk && dim2 === 26 && net === 255 && dim3 === 26 && q === 255 && asked,
    [dim1, stopOk, dim2, net, dim3, q, asked, stopEv.trim()].join());
}
console.log(failed ? failed + ' failed' : 'all passed');
process.exit(failed ? 1 : 0);
