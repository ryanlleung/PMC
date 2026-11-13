// Bench scenarios against the firmware build (pmc-wasm.js), in node:
//   node simulator/tests/run.js [--png <dir>]
// Each case boots a new board and checks the firmware's replies. The board
// sends nothing unasked, so a case reads the event log (EVENT?). The text
// port takes the remote command set only; homing, calibration and settings
// go through the simulator's test hooks (sim-node.js), as the screens do.
const { load, CFG } = require('./sim-node.js');
const fs = require('fs'), path = require('path');
const firmwareVersion = fs.readFileSync(path.join(__dirname, '../../InitialTesting/pmc_config.h'), 'utf8')
  .match(/#define\s+PMC_FW_VERSION\s+"([^"]+)"/)[1];
const pngDir = process.argv.includes('--png') ? process.argv[process.argv.indexOf('--png') + 1] : null;
let failed = 0;
function check(name, ok, detail) {
  console.log((ok ? 'ok   ' : 'FAIL ') + name + (ok || !detail ? '' : '\n     ' + detail));
  if (!ok) failed++;
}
// The last n events, newest last.
const events = (s, n = 100) => s.cmd('EVENT? ' + n, 2000);
// The physics plant has far more gain than the bench (0.8 to 12.5 mbar over 20 to 100 % open): tests of other
// things in AUTO on it run one PID, KP 4.00 TI 0.25, the factory to 0.16.17. The factory from 0.16.18 is the
// bench table (PID table On, 6/10/14 mbar, 0.06/0.60, 0.12/0.82, 0.80/0.94, 0.40/0.25).
// The PID table is always on (0.16.20): one PID means every set the same.
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
  check('bundle matches PMC_FW_VERSION', s.version() === firmwareVersion, s.version() + ' vs ' + firmwareVersion);
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
{ // Homing from the screen: Home valve (the Valve tile, 0.15.3), then Home in the question.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.tap(269, 184); s.tap(346, 147); s.run(120000);
  s.out += events(s, 10);
  // The current firmware defines count 0 at the independent Return point above the switch.
  const off = s.get(5) / 2 - s.get(30);
  check('home from the screen', /Homed in/.test(s.out) && s.get(30) === 0 && off === s.setting(CFG.RETURN), s.out.slice(-400));
  shot(s, 'homed');
}
{ // Start AUTO in MANUAL (Maintain to 0.15.2): AUTO at the pressure as shown, from where the valve is, no question.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000); s.run(400000);
  const p = s.cmd('PRESSURE?').match(/PRESSURE ([\d.]+) mbar/)[1], before = s.get(30);
  // The tiles and header follow within 0.15 s of the release (0.15.4; the 0.5 s refresh was too slow).
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
{ // The IP tile on Home (0.15.6; kept over the template's Trend in 0.15.9) shows the address/prefix
  // and opens System, as Menu > System does.
  const hdr = (s) => Buffer.from(new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 40).slice().buffer).toString('base64');
  const a = homed(load()), home = hdr(a);
  shot(a, 'home-ip');
  a.tap(415, 128); shot(a, 'home-ip-system');
  const b = homed(load());
  b.tap(415, 184); b.tap(84, 184);   // Menu > System (bottom left from 0.16.2)
  check('Home IP tile opens System', hdr(a) === hdr(b) && hdr(a) !== home);
}
{ // The Menu (0.16.2, Ryan 9 Nov: no Settings page, no row captions, an icon on every tile): nine tiles in three
  // rows of three, each opening its page in one tap; Review and save bottom right, dark blue (0.15.23).
  const s = homed(load());
  const screen = () => { const m = new Uint8Array(s.x.memory.buffer); let p = s.x.sim_screen(), e = p; while (m[e]) e++;
    return Buffer.from(m.subarray(p, e)).toString(); };
  // Home (its slot is the power icon on Home itself), then Menu: on the Menu that spot is its last tile.
  const menu = () => { if (screen() !== 'Home') s.tap(22, 20); s.tap(415, 184); };
  menu();
  check('Menu opens', screen() === 'Menu', screen());
  shot(s, 'menu');
  const tiles = [[84, 72, 'Trend'], [240, 72, 'Events'], [396, 72, 'PID tuning'], [84, 128, 'Motor settings'],
    [240, 128, 'Valve position'], [396, 128, 'Calibrate sensor'], [84, 184, 'System'], [240, 184, 'Diagnostics'],
    [396, 184, 'Review and save']];
  const wrong = tiles.map(([x, y, t]) => { menu(); s.tap(x, y); return [t, screen()]; })
    .filter(([t, got]) => t !== got).map(([t, got]) => t + ' -> ' + got);
  check('every Menu tile opens its page', wrong.length === 0, wrong.join(', '));
  menu(); s.tap(396, 184); const review = screen(); s.tap(66, 20);
  check('Back from a page returns to the Menu', review === 'Review and save' && screen() === 'Menu', review + ' then ' + screen());
  // Review and save is the one dark blue tile: white lettering on the Start AUTO blue, the others white cards.
  const px = (x, y) => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[y * 480 + x];
  check('Review and save tile dark blue, the others white', px(330, 168) !== 0xFFFF && px(18, 168) === 0xFFFF && px(174, 168) === 0xFFFF,
    [px(330, 168), px(18, 168), px(174, 168)].join());
  s.tap(240, 128); shot(s, 'valve-position'); s.tap(66, 20);
  s.tap(84, 128); shot(s, 'valve-settings'); s.tap(66, 20);
  // Choose sensor is gone (one sensor per board): no CFG_SENSOR key (27 to 0.16.13; set 4's KP from 0.16.14, the
  // Disable switch edges 31 and 32 from 0.16.21, the keys end at 33), the sensor range is the 15 psia Druck's, and
  // the old Settings slot now opens Review and save.
  check('no sensor-in-use setting', s.setting(33) === -2147483648 && s.setting(CFG.DBC) === 8 && s.setting(CFG.B3) === 140 &&
    s.setting(CFG.PASS_LO) === 5 && s.setting(CFG.PASS_HI) === 50,
    [33, CFG.DBC, CFG.B3, CFG.PASS_LO, CFG.PASS_HI].map(k => s.setting(k)).join(' '));
  check('setpoint limit is the 15 psia range', s.cmd('SETPOINT 1035') === 'ERR 102 Outside 0 to 1034.2 mbar\r\n', s.cmd('SETPOINT 1035'));
  menu(); s.tap(386, 184); check('old Settings slot opens Review and save', screen() === 'Review and save', screen());
  // Motor override moved from Valve position to Motor settings' fourth tab, Override (0.16.17, Ryan 12 Nov: "it
  // doesnt feel right to be in valve position"): asked first, and Back returns to Motor settings on that tab.
  // Its button is at the right of the second row from 0.16.21 (the Disable switch toggle and edges before it).
  menu(); s.tap(84, 128); s.tap(305, 72); shot(s, 'motor-settings-override'); s.tap(298, 184);
  const asked = s.popup(); s.tap(318, 147); const opened = screen();
  s.tap(66, 20); const back = screen(); s.tap(298, 184); const again = s.popup(); s.tap(160, 147);
  check('Motor override: on Motor settings > Override, asked first, Back to that tab',
    asked && opened === 'Motor override' && back === 'Motor settings' && again && screen() === 'Motor settings',
    [asked, opened, back, again, screen()].join());
}
{ // PID tile opens PID tuning; KD is a saved setting from its keypad.
  const s = homed(load());
  shot(s, 'home-pid');
  s.tap(415, 72); shot(s, 'pid-tuning');
  s.tap(298, 184); s.tap(40, 239); s.tap(250, 239); s.tap(110, 183); s.tap(320, 212);   // set 1's KD (the set row), 1.5, Set
  s.tap(415, 128); s.tap(319, 138);   // Save settings, then Save in the question
  check('KD saves from the PID tuning page', s.setting(CFG.KD) === 150, 'KD ' + s.setting(CFG.KD));
  // Gain schedule on (0.15.15): the PID tab (Gains from 0.16.25) shows the set row; Set 3's KP from its card. Four
  // sets from 0.16.14; from 0.16.25 four cards of 82 px at x 10, 97, 184 and 271.
  s.cmd('SCHED SET PV,6,10,14,15', 1000); s.run(1000);
  shot(s, 'pid-tuning-sets');
  s.tap(221, 128); s.tap(62, 184); s.tap(40, 239); s.tap(250, 239); s.tap(110, 183); s.tap(320, 212);   // Set 3, KP 1.5, Set
  s.tap(415, 128); s.tap(319, 138);
  check('Set 3 KP saves from the set row', s.setting(CFG.KP3) === 150 && s.setting(CFG.KP) === 6 && s.setting(CFG.KP2) === 12 &&
    s.setting(CFG.KP4) === 40, [s.setting(CFG.KP), s.setting(CFG.KP2), s.setting(CFG.KP3), s.setting(CFG.KP4)].join());
  shot(s, 'pid-tuning-set3');
  s.tap(304, 128); s.tap(62, 184); s.tap(40, 239); s.tap(250, 239); s.tap(320, 212);   // Set 4, KP 1., Set
  s.tap(415, 128); s.tap(319, 138);
  check('Set 4 KP saves from the set row', s.setting(CFG.KP4) === 100 && s.setting(CFG.KP3) === 150,
    [s.setting(CFG.KP3), s.setting(CFG.KP4)].join());
  shot(s, 'pid-tuning-set4');
  // Set ranges (the PID table tab to 0.16.24; 0.16.25, Fable's review): B1, B2 and B3 in a row, the hysteresis
  // under B1. B1 from its card.
  s.tap(182, 72); s.run(500);
  shot(s, 'pid-tuning-table');
  s.tap(65, 128); s.tap(110, 183); s.tap(320, 212);   // Set 1 | 2 (B1), 5, Set
  s.tap(415, 128); s.tap(319, 138);
  check('Set ranges: B1 saves from the first card', s.setting(CFG.B1) === 50 && s.setting(CFG.B2) === 100, s.setting(CFG.B1));
  // AUTO (AUTO limits to 0.16.24): AUTO min and Pressure response, the setpoint band under AUTO min.
  s.tap(291, 72); s.run(500);
  shot(s, 'pid-tuning-auto');
  s.tap(65, 184); s.tap(40, 239); s.tap(320, 212);   // SP band, 1, Set
  s.tap(415, 128); s.tap(319, 138);
  check('AUTO: the setpoint band saves from the second row', s.setting(CFG.BAND) === 100, s.setting(CFG.BAND));
}
{ // A tap on anything greyed round a popup closes it, as Back (0.15.16, Ryan 5 Nov: "Make popups exitable when
  // tapping the greyed out areas"): the dimmed page, the greyed strip, the greyed header icons. A tap on the box does not.
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
  s.tap(455, 100); const sc = !s.popup();                           // the dimmed page right of the box
  s.tap(24, 20); s.tap(100, 240); const sd = !s.popup();            // the greyed strip
  s.tap(24, 20); s.tap(24, 20); const se = !s.popup();              // the greyed power icon
  check('Shut down question: closes on the dimmed page, the greyed strip and the greyed power icon, not on its box; nothing done',
    sa && sb && sc && sd && se && s.cmd('MODE?') === 'MODE MANUAL\r\n', [sa, sb, sc, sd, se].join() + ' ' + s.cmd('MODE?'));
  s.tap(415, 72); s.tap(182, 72); s.tap(94, 128); const pa = s.popup();   // PID tuning > Schedule > Schedule on: the list
  s.tap(100, 54); const pb = s.popup();                             // its caption
  shot(s, 'popup-pick');
  s.tap(415, 200);                                                  // the dimmed page (Discard changes under it)
  check('pick list: stays on a tap on its caption, closes on the dimmed page; nothing picked',
    pa && pb && !s.popup() && s.setting(CFG.SCHED) === 2, [pa, pb, s.popup(), s.setting(CFG.SCHED)].join());
  s.tap(415, 72); const ha = s.popup();                             // Help
  s.tap(100, 240); const hb = !s.popup();                           // the greyed strip under it
  check('Help: closes on a tap on the greyed strip', ha && hb, [ha, hb].join());
}
{ // The remote light holds steady while a client polls (0.15.17, Ryan: "The flashing arrow is a bit annoying"):
  // each arrow lit until 3 s after its last message, not 300 ms a message.
  const s = homed(load()); s.run(12000);
  const px = (x, y) => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[y * 480 + x];
  const arrows = () => [px(324, 16), px(336, 24)];   // the arrow down's and the arrow up's stems
  let dark = 0, n = 0;
  for (let i = 0; i < 8; i++) {
    s.send('PRESSURE?');
    for (let t = 0; t < 10; t++) { s.run(100); if (i || t > 2) { n++; if (arrows().some(c => c !== 0xFFFF)) dark++; } }
  }
  shot(s, 'remote-light');
  check('remote light: steady while polled once a second', n > 70 && dark === 0, dark + ' of ' + n + ' samples not lit');
  s.run(1800); const still = arrows().every(c => c === 0xFFFF);
  s.run(1500); const grey = arrows().every(c => c !== 0xFFFF);
  check('remote light: grey 3 s after the last message', still && grey, [still, grey].join());
}
{ // Set position (no homing): Set as 0 % takes the count as 0 where it stands.
  const s = load(); s.boot(); s.run(2000);
  const ok = s.setClosed(), st = s.cmd('MODE?;VALVE?');
  check('set position by hand reads 0.00 %', ok && st === 'MODE MANUAL;VALVE 0.00 %\r\n', s.why() + ' ' + st);
}
{ // Set as 0 % after homing reads 0.00 %, not the seat (Ryan, 4 Nov).
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
{ // F-2: excitation out of 9.5-10.5 V.
  const s = homed(load()); s.set(10, 9.2); s.run(5000);
  const ev = events(s, 5);
  check('F-2 excitation low', /F-2/.test(ev), ev);
}
{ // F-3: the switch never trips during homing.
  const s = load(); s.boot(); s.set(5, 40000); s.set(7, 2); s.run(2000);
  s.home(400000);
  const out = events(s, 5);
  check('F-3 limit not found', /F-3/.test(out), out.slice(-600));
}
{ // F-4: homing from the switch, which stays pressed after the clearance move.
  const s = homed(load()); s.set(7, 1);
  s.home(60000);
  const out = events(s, 5);
  check('F-4 limit stuck', /F-4/.test(out), out.slice(-600));
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
{ // A large setpoint change in AUTO applies at once from the SP keypad: no question (0.14.22).
  const s = homed(load());
  s.cmd('SETPOINT 5;MODE AUTO'); s.run(5000);
  s.tap(121, 184); s.tap(40, 239); s.tap(110, 183); s.tap(320, 212);   // SP box, 1, 5, Set
  s.run(500);
  shot(s, 'sp-change');
  const st = s.cmd('SETPOINT?;MODE?');
  check('SP 5 to 15 mbar in AUTO applies without a question', st === 'SETPOINT 15.0 mbar;MODE AUTO\r\n', st);
}
{ // The motor supply is no longer sensed: counted moves can continue without physical motion.
  const s = homed(load());
  s.cmd('SETPOINT 5;MODE AUTO'); s.run(400000);
  s.set(12, 0);
  const needle = s.get(5), count = s.get(30);
  s.cmd('SETPOINT 8'); s.run(5000);
  check('motor power loss does not directly end AUTO', s.cmd('MODE?;ALARM?') === 'MODE AUTO;ALARM NONE\r\n');
  check('unpowered motor stays still while the step count advances', s.get(5) === needle && s.get(30) !== count);
}
{ // F-6: the needle slips in AUTO, so the pressure does not follow.
  const s = oldGains(homed(load()));
  s.cmd('SETPOINT 5'); s.cmd('MODE AUTO'); s.run(300000);
  s.set(6, 0); s.cmd('SETPOINT 8');
  s.run(600000);
  const ev = events(s, 10);
  check('F-6 no response', /F-6/.test(ev), ev);
  shot(s, 'f06');
  // Fault detail: its buttons sit in the strip's row, so the strip hides (0.14.13).
  s.tap(415, 184); s.tap(240, 72); s.tap(180, 72);   // Menu > Events (0.16.2), the fault
  shot(s, 'f06-detail');
  s.tap(181, 240);   // Reset fault
  check('fault detail Reset fault works', s.cmd('ALARM?') === 'ALARM NONE\r\n', s.cmd('ALARM?'));
}
{ // W-2: the rotary pump stops in AUTO; the pressure never comes back into band.
  const s = oldGains(homed(load()));
  s.cmd('SETPOINT 5'); s.cmd('MODE AUTO'); s.run(300000);
  s.set(4, 0);
  s.run(1900000);
  const ev = events(s, 10), st = s.cmd('MODE?;ALARM?');
  check('W-2 to MANUAL', /W-2/.test(ev) && /^MODE MANUAL;ALARM W-2,1,/.test(st), ev + st);
  shot(s, 'w02');
}
{ // W-3: controller hot.
  const s = homed(load()); s.set(13, 57); s.run(3000);
  const ev = events(s, 3);
  check('W-3 controller hot', /W-3,Controller hot/.test(ev) && /^ALARM W-3,0,Controller hot\r\n$/.test(s.cmd('ALARM?')), ev);
  shot(s, 'w03');
}
{ // Settings and the saved position survive a power cycle (serial flash).
  const s = homed(load());
  s.cmd('VALVE SET 10', 20000); const saved = s.save(CFG.KP, 300); s.run(5000);
  const flash = s.flash(), needle = s.get(5);
  const t = load(); t.x.sim_bench_init(); t.boot(flash); t.set(5, needle); t.run(3000);
  check('settings kept over a power cycle', saved && t.setting(CFG.KP) === 300, s.why() + ' KP ' + t.setting(CFG.KP));
  check('valve position offered after a power cycle', Math.abs(t.offer() - 6605) <= 1, 'offer ' + t.offer());
  // The question needs an answer: a tap round it, on the strip or on the power icon leaves it (0.15.16).
  t.turnOn(5000); const asked = t.popup();
  t.tap(455, 100); t.tap(100, 240); t.tap(24, 20);
  check('saved position question stays on a tap round it', asked && t.popup() && Math.abs(t.offer() - 6605) <= 1,
    [asked, t.popup(), t.offer()].join());
}
{ // Limit override is a two-finger deadman (0.13.7): held alone it does nothing; held, a second
  // finger on Close closes past a pressed switch, on Open opens; either finger lifting stops it.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.tap(415, 184); s.tap(84, 128); s.tap(305, 72); s.tap(298, 184); s.tap(318, 147);   // Menu > Motor settings > Override tab (0.16.17) > Motor override > Open
  s.set(7, 1); s.run(500);                                              // switch stuck pressed
  s.tap(186, 184, 1000);
  const c0 = s.get(30), blocked = c0 === 0;                             // Close alone refused
  const over = (d) => s.x.sim_touch(64, 184, d), second = (px, d) => s.x.sim_touch2(px, 184, d);
  over(1); s.run(2000); const alone = s.get(30);
  check('limit override alone does nothing', blocked && alone === c0 && !s.busy(), 'count ' + alone);
  second(415, 1); s.run(1000); second(415, 0); s.run(200);              // second finger on Set zero
  check('second finger elsewhere does nothing', s.get(30) === c0, 'count ' + s.get(30));
  second(186, 1); s.run(3000); const held = s.get(30);
  check('override + Close closes past the switch', held < -500, 'count ' + held);
  shot(s, 'override');
  second(186, 0); s.run(300); const rel = s.get(30); s.run(1000);
  check('lifting the second finger stops at once', s.get(30) === rel && !s.override(), s.get(30) + ' vs ' + rel);
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
  check('room air point refused without the sensor', !r && /no room pressure sensor/i.test(u.why()), u.why());
  check('typed air point still works', u.cal(2, 1013), u.why());
}
{ // The remote set: the reply rules of the handover page.
  const s = homed(load());
  const one = c => s.cmd(c, 1000);
  // R1, R2, R5: no echo; a query gives one line starting with its own words.
  const qs = ['PRESSURE?', 'SETPOINT?', 'MODE?', 'VALVE?', 'VALVE MOVING?', 'BAROMETER?', 'ALARM?', 'NETWORK?', 'TIME?'];
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
  check('BAROMETER? and NETWORK? in the simulator', one('BAROMETER?;NETWORK?') === 'BAROMETER 1013.20 mbar;NETWORK NONE\r\n', one('BAROMETER?;NETWORK?'));
  check('*IDN?', /^MercShore,iPC,[0-9A-F]{24},\d+\.\d+\.\d+\r\n$/.test(one('*IDN?')), one('*IDN?'));
  // EVENT?: the count line, then that many lines, newest first.
  const ev = s.cmd('EVENT? 3', 2000).split('\r\n');
  check('EVENT? 3', ev[0] === 'EVENT 3' && ev.slice(1, 4).every(l => /^\d+\.\d,[A-Z0-9-]+,/.test(l)) && ev.length === 5 && ev[4] === '',
    ev.join(' | '));
  check('EVENT? defaults to 1', s.cmd('EVENT?', 2000).split('\r\n').length === 3);
  // R8: ERR <code> <text>, the first refusal ends the line.
  const errs = [['NO SUCH', 100], ['SETPOINT x', 101], ['SETPOINT 2000', 102], ['X'.repeat(97), 103], ['MODE?;EVENT? 2', 104],
    ['EVENT? 0', 102], ['VALVE SET 101', 102], ['VALVE SET', 101], ['MODE', 101], ['TIME SET 2026-13-01 00:00:00', 101],
    ['PRESSURE? 5', 100], ['pressure?', 100], ['SETPOINT', 101], ['SETPOINTS 5', 100], ['EVENT?5', 100],
    // 0.16.12: the four-letter names to 0.16.11 are not taken any more.
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
{ // Remote control switch: On by default; Off, the link takes queries only; On from the screen, it takes sets.
  const d = load(); d.boot(); d.run(1000);   // boot() leaves the switch as the firmware starts it
  check('switch On by default', d.cmd('SETPOINT 5;SETPOINT?') === 'SETPOINT 5.0 mbar\r\n', d.cmd('SETPOINT?'));
  const s = load(); s.boot(undefined, { remote: false }); s.set(5, 40000); s.run(2000);
  check('switch Off: SETPOINT refused', s.cmd('SETPOINT 5') === 'ERR 211 Read only over the network\r\n', s.cmd('SETPOINT 5'));
  check('switch Off: STOP refused', s.cmd('STOP') === 'ERR 211 Read only over the network\r\n');
  check('switch Off: queries answer', s.cmd('MODE?;SETPOINT?') === 'MODE NOT HOMED;SETPOINT NONE\r\n', s.cmd('MODE?;SETPOINT?'));
  s.tap(415, 184); s.tap(84, 184); s.tap(357, 72);    // Menu, System (0.16.2: bottom left), Remote control (top right from 0.16.24)
  check('switch On from the screen: SETPOINT taken', s.cmd('SETPOINT 5;SETPOINT?') === 'SETPOINT 5.0 mbar\r\n', s.cmd('SETPOINT?'));
  check('switch On is a NET event', /NET,Remote control on \(screen\)/.test(events(s, 5)), events(s, 5));
  shot(s, 'system');
  s.tap(357, 72);
  check('switch Off again: refused', s.cmd('SETPOINT 6') === 'ERR 211 Read only over the network\r\n');
  check('switch Off: PID SET refused, PID? answers', s.cmd('PID SET 3,0.5') === 'ERR 211 Read only over the network\r\n' &&
    s.cmd('PID?') === 'PID 0.06,0.60,0.00\r\n', s.cmd('PID?'));
  check('switch Off: PIDS SET and SCHED SET refused, PIDS? and SCHED? answer',
    s.cmd('PIDS SET 2,3,0.5') === 'ERR 211 Read only over the network\r\n' &&
    s.cmd('SCHED SET PV,6,10,14,15') === 'ERR 211 Read only over the network\r\n' &&
    s.cmd('PIDS?;SCHED?') === 'PIDS 1,0.06,0.60,0.00,0.12,0.82,0.00,0.80,0.94,0.00,0.40,0.25,0.00;SCHED PV,6.0,10.0,14.0,15\r\n',
    s.cmd('PIDS?;SCHED?'));
}
{ // Homing return: its own setting, the step 4 move off the switch; 0 % is there.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  check('return set to 150', s.save(CFG.RETURN, 150) && s.setting(CFG.RETURN) === 150, s.why());
  s.home();
  const ev = events(s, 3);
  check('homed with the switch 150 steps below 0 %', /Homed in \d+ s, switch at -150, 0 % at 0/.test(ev) && s.get(5) === 300, ev + ' needle ' + s.get(5));
}
// Shut down and OFF (0.15.0; 0.15.1: to 0 %, not onto the switch, and
// every start OFF): the valve goes to 0 %, coils off; moves refused until a
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
  check('start: BOOT event says OFF', /BOOT,Power-up \(event only\), OFF, valve not homed/.test(events(s, 5)), events(s, 5));
  shot(s, 'boot-off');
  s.tap(240, 136, 100); s.tap(240, 136, 150);
  check('turning on: the logo on white', px(s, 10, 10) === 0xFFFF, px(s, 10, 10));
  shot(s, 'turning-on');
  s.run(3000);
  check('double tap at start: NOT HOMED', s.cmd('MODE?') === 'MODE NOT HOMED\r\n', s.cmd('MODE?'));
  shot(s, 'turned-on');
}
{ // System (0.16.24, Ryan 13 Nov): Cryostat name and Remote control, IP address and MAC, Firmware, Restart and
  // Shut down; no Diagnostics button (it is on the Menu). Shut down asks the power icon's question, Restart its own.
  const hdr = (s) => Buffer.from(new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 40).slice().buffer).toString('base64');
  const s = homed(load());
  s.tap(415, 184); s.tap(84, 184); const sys = hdr(s);
  s.tap(415, 184); const shutQ = s.popup();
  s.tap(318, 165, 3000);
  check('System > Shut down: the power question, then shutting down', shutQ && /^MODE (SHUTDOWN|OFF)/.test(s.cmd('MODE?')),
    [shutQ, s.cmd('MODE?')].join());
  const r = homed(load());
  r.tap(415, 184); r.tap(84, 184); r.tap(298, 184); const restartQ = r.popup();
  r.tap(160, 147);   // Cancel
  check('System > Restart: asks, Cancel leaves it running', restartQ && !r.popup() && r.cmd('MODE?') === 'MODE MANUAL\r\n',
    [restartQ, r.cmd('MODE?')].join());
  const n = homed(load());
  n.tap(415, 184); n.tap(84, 184); n.tap(120, 128); const ip = hdr(n); shot(n, 'system-ip');
  check('System: IP address on the second row left opens its page', ip !== sys);
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
  check('shut down event', /OFF,Valve at 0 %, count 0; coils off, OFF/.test(events(s, 5)), events(s, 5));
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
{ // Close the valve unticked (0.15.7): OFF at once, the valve left open, its count kept.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000); s.run(2000);
  const needle = s.get(5);
  s.tap(24, 20); s.tap(240, 111); shot(s, 'shutdown-leave');
  s.tap(318, 165, 1000);
  check('shut down, valve left: OFF at 30 %', s.cmd('MODE?;VALVE?;VALVE MOVING?') === 'MODE OFF;VALVE 30.00 %;VALVE MOVING 0\r\n' &&
    s.get(5) === needle && s.get(15) === 0, s.cmd('MODE?;VALVE?') + ' needle ' + s.get(5) + ' coils ' + s.get(15));
  check('shut down, valve left: event', /OFF,Shut down, valve left at 30\.0 %: coils off, OFF/.test(events(s, 5)), events(s, 5));
  const t = load(); t.boot(s.flash(), { off: true }); t.set(5, needle); t.run(2000);
  check('shut down, valve left: kept at the next start', t.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE 30.00 %\r\n', t.cmd('MODE?;VALVE?'));
  const u = homed(load());
  u.tap(24, 20); u.tap(240, 111); u.tap(240, 111); u.tap(318, 165, 1000);
  check('ticked again: closes', u.cmd('MODE?') === 'MODE OFF\r\n' && /at 0 %: coils off, OFF/.test(events(u, 5)), events(u, 5));
}
{ // Power-up after Shut down, the switch released: OFF, the count kept; on, it moves at once.
  for (const r of [0, 1, 2]) {
    const s = load(); s.boot(parkedFlash, { off: true, reset: r }); s.set(5, NEEDLE0); s.run(2000);
    const what = ['power-up', 'watchdog', 'Restart'][r];
    check('start after Shut down (' + what + '): OFF at 0 %', s.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE 0.00 %\r\n' &&
      s.offer() === -2147483648, s.cmd('MODE?;VALVE?') + ' offer ' + s.offer());
    if (r) continue;
    check('start after Shut down: event', /REF,Kept from the last shut down: count 0 \(0\.0 %\)\r?\n/.test(events(s, 5)), events(s, 5));
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
    /Kept from the last shut down: count 0 \(0\.0 %\), not homed/.test(events(t, 5)), t.cmd('VALVE?') + events(t, 5));
}
{ // Switched off without Shut down: the saved position offered, and the log says so.
  const s = homed(load());
  s.cmd('VALVE SET 30', 60000); s.run(3000);
  const t = load(); t.boot(s.flash()); t.set(5, 40000); t.run(2000);
  check('switched off without Shut down', t.offer() === Math.round(66048 * 0.3) && /Last power-off without Shut down/.test(events(t, 5)),
    t.offer() + '\n' + events(t, 5));
  shot(t, 'unparked-ask');
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
  // The switch question needs an answer: a tap round it or on the strip leaves it (0.15.16).
  const asked = s.popup();
  s.tap(455, 100); s.tap(100, 240);
  check('switch question stays on a tap round it', asked && s.popup() && /W-1/.test(s.cmd('ALARM?')), [asked, s.popup(), s.cmd('ALARM?')].join());
}
{ // Already at 0 %: OFF at once.
  const s = homed(load());
  check('at 0 %: OFF at once', s.shutdown(300) && s.cmd('MODE?;VALVE?') === 'MODE OFF;VALVE 0.00 %\r\n', s.why() + s.cmd('MODE?;VALVE?'));
}
{ // PID? and PID SET (0.15.8): the PID tab's Save over the text port, also in AUTO.
  const s = homed(load());
  const one = c => s.cmd(c, 1000);
  check('PID? defaults', one('PID?') === 'PID 0.06,0.60,0.00\r\n', one('PID?'));
  check('PID SET kp,ti silent, PID? confirms', one('PID SET 3.5,0.5') === '' && one('PID?') === 'PID 3.50,0.50,0.00\r\n', one('PID?'));
  check('PID SET kp,ti,kd', one('PID SET 3.5, 0.5, 1.25;PID?') === 'PID 3.50,0.50,1.25\r\n', one('PID?'));
  check('PID SET saves the settings', s.setting(CFG.KP) === 350 && s.setting(CFG.TI) === 50 && s.setting(CFG.KD) === 125,
    [s.setting(CFG.KP), s.setting(CFG.TI), s.setting(CFG.KD)].join());
  const bad = [['PID SET', 101], ['PID SET 3', 101], ['PID SET 3,0.5,1,2', 101], ['PID SET x,1', 101], ['PID SET 3,,1', 101],
    ['PID SET 0,0.5', 102], ['PID SET 101,0.5', 102], ['PID SET 3,-1', 102], ['PID SET 3,0.5,101', 102]];
  const wrong = bad.map(([c, n]) => [c, n, one(c)]).filter(([c, n, r]) => !new RegExp('^ERR ' + n + ' [A-Z][^;\\r\\n]*\\r\\n$').test(r))
    .map(([c, n, r]) => c + ' -> ' + JSON.stringify(r) + ' (want ' + n + ')');
  check('PID SET refusals', wrong.length === 0, wrong.join('\n     '));
  check('PID SET range text', one('PID SET 0,0.5') === 'ERR 102 Outside 0.01 to 100.00 for KP\r\n', one('PID SET 0,0.5'));
  check('refused PID SET changes nothing', one('PID?') === 'PID 3.50,0.50,1.25\r\n', one('PID?'));
  s.cmd('VALVE SET 30', 60000);
  s.cmd('SETPOINT 3;MODE AUTO', 20000);
  check('PID SET in AUTO, AUTO kept', one('PID SET 2,0.4;PID?;MODE?') === 'PID 2.00,0.40,1.25;MODE AUTO\r\n', one('PID?;MODE?'));
  check('PID SET in AUTO saves', s.setting(CFG.KP) === 200 && s.setting(CFG.TI) === 40, s.setting(CFG.KP) + ' ' + s.setting(CFG.TI));
}
{ // Gain schedule (0.15.15, Ryan 5 Nov "build like industry ones"): PIDS?, PIDS SET, SCHED?, SCHED SET,
  // PID? and PID SET on the set in use, and AUTO switching sets on the pressure with hysteresis.
  const s = homed(load());
  const one = c => s.cmd(c, 1000);
  check('PIDS? defaults: the bench table (0.16.18)', one('PIDS?') === 'PIDS 1,0.06,0.60,0.00,0.12,0.82,0.00,0.80,0.94,0.00,0.40,0.25,0.00\r\n',
    one('PIDS?'));
  check('SCHED? defaults: on the pressure, 6, 10 and 14 mbar, 15 %', one('SCHED?') === 'SCHED PV,6.0,10.0,14.0,15\r\n', one('SCHED?'));
  check('PIDS SET silent, PIDS? confirms', one('PIDS SET 2,5,0.3') === '' && one('PIDS SET 3, 6, 0.35, 0.5') === '' &&
    one('PIDS SET 4,7,0.4') === '' &&
    one('PIDS?') === 'PIDS 1,0.06,0.60,0.00,5.00,0.30,0.00,6.00,0.35,0.50,7.00,0.40,0.00\r\n', one('PIDS?'));
  check('PIDS SET saves the settings', s.setting(CFG.KP2) === 500 && s.setting(CFG.TI2) === 30 && s.setting(CFG.KD3) === 50 &&
    s.setting(CFG.KP4) === 700 && s.setting(CFG.TI4) === 40, [s.setting(CFG.KP2), s.setting(CFG.TI2), s.setting(CFG.KD3),
    s.setting(CFG.KP4), s.setting(CFG.TI4)].join());
  const bad = [['PIDS SET', 101], ['PIDS SET 2', 101], ['PIDS SET 2,1', 101], ['PIDS SET x,1,1', 101], ['PIDS SET 2,1,1,1,1', 101],
    ['PIDS SET 0,1,1', 102], ['PIDS SET 5,1,1', 102], ['PIDS SET 2,0,1', 102], ['PIDS SET 3,1,-1', 102],
    ['SCHED SET', 101], ['SCHED SET ON,6,10,14,15', 101], ['SCHED SET PV,6,10,14', 101], ['SCHED SET PV,6,20,15', 101],
    ['SCHED SET PV,6,10,14,15,1', 101], ['SCHED SET PV,x,10,14,15', 101], ['SCHED SET PV,0,10,14,15', 102],
    ['SCHED SET PV,6,10,14,41', 102], ['SCHED SET PV,20,6,30,15', 102], ['SCHED SET PV,10,11,20,15', 102],
    ['SCHED SET PV,6,10,11,15', 102], ['SCHED SET PV,6,10,-1,15', 102]];
  const wrong = bad.map(([c, n]) => [c, n, one(c)]).filter(([c, n, r]) => !new RegExp('^ERR ' + n + ' [A-Z][^;\\r\\n]*\\r\\n$').test(r))
    .map(([c, n, r]) => c + ' -> ' + JSON.stringify(r) + ' (want ' + n + ')');
  check('PIDS SET and SCHED SET refusals', wrong.length === 0, wrong.join('\n     '));
  check('SCHED SET boundaries text', one('SCHED SET PV,20,6,30,15') === 'ERR 102 B2 must be above B1, by more than the hysteresis either side\r\n' &&
    one('SCHED SET PV,6,10,11,15') === 'ERR 102 B3 must be above B2, by more than the hysteresis either side, or 0 for three sets\r\n',
    one('SCHED SET PV,20,6,30,15') + one('SCHED SET PV,6,10,11,15'));
  check('refusals change nothing', one('SCHED?;PIDS?') ===
    'SCHED PV,6.0,10.0,14.0,15;PIDS 1,0.06,0.60,0.00,5.00,0.30,0.00,6.00,0.35,0.50,7.00,0.40,0.00\r\n', one('SCHED?;PIDS?'));
  check('SCHED SET PV, B3 0: three sets', one('SCHED SET PV,6,20,0,15') === '' && one('SCHED?') === 'SCHED PV,6.0,20.0,0.0,15\r\n' &&
    s.setting(CFG.SCHED) === 2 && s.setting(CFG.B1) === 60 && s.setting(CFG.B2) === 200 && s.setting(CFG.B3) === 0, one('SCHED?'));
  // The sim plant runs about 0.8 to 12.5 mbar (20 to 100 % open), so the
  // boundaries here are 3, 6 and 8 mbar with 10 %: up at 3.3, 6.6 and 8.8,
  // down at 7.2, 5.4 and 2.7. Sim-plant gains, a different set each so PID?
  // shows which is in use.
  one('SCHED SET PV,3,6,8,10;PIDS SET 1,6,0.25;PIDS SET 2,5,0.25,0');
  one('PIDS SET 3,4.5,0.25,0;PIDS SET 4,4,0.25,0');
  // The set changes so far, oldest first (EVENT? lists the newest first).
  const sws = () => [...events(s, 100).matchAll(/AUTO PID set (\d) to (\d), pressure ([\d.]+) mbar/g)].map(m => [+m[1], +m[2], +m[3]]).reverse();
  s.cmd('VALVE SET 40', 60000);
  s.run(240000);
  s.cmd('SETPOINT 10;MODE AUTO', 1000);
  check('AUTO starts on the set for the lower of pressure and setpoint (3.8 mbar: set 2)', /Auto at setpoint 10\.0 mbar, valve [\d.]+ %, PID set 2/.test(events(s, 3)),
    events(s, 3));
  s.run(600000);
  check('PV schedule: set 4 at 10 mbar', /^PIDS 4,/.test(one('PIDS?')) && one('PID?') === 'PID 4.00,0.25,0.00\r\n',
    one('PRESSURE?;PIDS?;PID?'));
  const first = sws();
  check('then 2 to 3 over 6.6 and 3 to 4 over 8.8 mbar (B x 1.1), once each', first.length === 2 && first[0][0] === 2 &&
    first[0][1] === 3 && first[0][2] >= 6.6 && first[0][2] < 7.5 && first[1][0] === 3 && first[1][1] === 4 && first[1][2] >= 8.8 &&
    first[1][2] < 9.7, JSON.stringify(first));
  // PID SET edits the set in use.
  check('PID SET edits the set in use', one('PID SET 3.8,0.25;PID?') === 'PID 3.80,0.25,0.00\r\n' && s.setting(CFG.KP4) === 380 &&
    s.setting(CFG.KP3) === 450 && s.setting(CFG.KP) === 600, [s.setting(CFG.KP4), s.setting(CFG.KP3), s.setting(CFG.KP)].join());
  s.cmd('SETPOINT 2', 1000);
  s.run(900000);
  // Going down the set follows the setpoint at once (0.16.9: the lower of
  // pressure and setpoint), so a big step down does not start on set 4.
  const sw = sws().slice(2);
  check('10 -> 2: set 4 to 1 at once, still at 10 mbar (the setpoint\'s set), no other change', sw.length === 1 && sw[0][0] === 4 &&
    sw[0][1] === 1 && sw[0][2] > 9, JSON.stringify(sw));
  check('10 -> 2: AUTO kept, near 2 mbar, set 1', one('MODE?') === 'MODE AUTO\r\n' && Math.abs(parseFloat(one('PRESSURE?').slice(9)) - 2) < 0.3 &&
    /^PIDS 1,/.test(one('PIDS?')) && one('PID?') === 'PID 6.00,0.25,0.00\r\n', one('MODE?;PRESSURE?;PIDS?;PID?'));
  s.cmd('SETPOINT 10', 1000);
  s.run(900000);
  const up = sws().slice(3);
  check('2 -> 10: set 1 to 2 over 3.3, 2 to 3 over 6.6, 3 to 4 over 8.8 mbar', up.length === 3 && up[0][0] === 1 && up[0][1] === 2 &&
    up[0][2] >= 3.3 && up[0][2] < 4.3 && up[1][0] === 2 && up[1][1] === 3 && up[1][2] >= 6.6 && up[1][2] < 7.6 &&
    up[2][0] === 3 && up[2][1] === 4 && up[2][2] >= 8.8 && up[2][2] < 9.8, JSON.stringify(up));
  shot(s, 'gain-schedule');
  // The setpoint schedule is gone (0.15.20, Ryan 6 Nov): SP is refused, the table stays on the pressure.
  check('SCHED SET SP refused', one('SCHED SET SP,3,6,8,10') === 'ERR 101 Expected: SCHED SET PV,<b1>,<b2>,<b3>,<h>\r\n' &&
    one('SCHED?') === 'SCHED PV,3.0,6.0,8.0,10\r\n', one('SCHED SET SP,3,6,8,10') + one('SCHED?'));
  // The PID table is always on (0.16.20, Ryan 12 Nov: "remove the set 1 pid value function. always has pid table").
  check('SCHED SET OFF refused, the table stays on', one('SCHED SET OFF,6,10,14,15') ===
    'ERR 102 The PID table is always on: give PV\r\n' && one('SCHED?') === 'SCHED PV,3.0,6.0,8.0,10\r\n',
    one('SCHED SET OFF,6,10,14,15') + one('SCHED?'));
}
{ // The limit switch pressed away from the seat (0.16.4, Ryan PR #43): a yellow edge round Home's Valve tile at
  // once (the "Limit" tag of 0.16.1 gone) and W-4 within 0.3 s (two 1 s reads to 0.16.3); released, the edge goes.
  const s = homed(load()); s.cmd('VALVE SET 20', 60000);
  const px = (x, y) => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[y * 480 + x];
  const before = px(187, 100);
  s.set(7, 1); s.run(200);   // the wire open: the NC contact reads pressed
  const edge = px(187, 100), al = s.cmd('ALARM?', 100);
  shot(s, 'limit-edge');
  s.set(7, 0); s.run(300);
  const after = px(187, 100);
  check('limit pressed: yellow edge on the Valve tile, W-4 within 0.3 s', before === 0xFFFF && edge === 0xF606 && /W-4/.test(al) &&
    after === 0xFFFF, [before.toString(16), edge.toString(16), al.trim(), after.toString(16)].join(' '));
}
{ // Disable switch (Ignore switch in 0.16.21), the pass band (0.16.5, Ryan PR #43: the force switch trips about 13 % where the valve binds): a
  // toggle on Motor settings > Override (0.16.19; Motor override > Switch to 0.16.18; "Switch pass" to 0.16.20);
  // between From and To, 5 and 50 % (10 to 20 % until 0.16.8, 5 to 25 % until 0.16.11), the pressed switch neither
  // stops closing nor raises W-4; below From it stops.
  const s = homed(load()); s.cmd('VALVE SET 40', 150000);
  s.tap(415, 184); s.tap(84, 128); s.tap(305, 72); s.tap(180, 128);   // Menu > Motor settings > Override, Disable switch on
  shot(s, 'override-switch');
  const ev1 = events(s, 1), on1 = /Limit switch disabled from 5 to 50 % open: on/.test(ev1);
  s.tap(180, 128); const off = /Limit switch disabled from 5 to 50 % open: off/.test(events(s, 1));   // a second press: off
  s.tap(180, 128);                                                                                    // and on again
  const on = on1 && off && /Limit switch disabled from 5 to 50 % open: on/.test(events(s, 1));
  check('Disable switch: on, off and on again from the toggle', on, ev1.trim());
  s.set(7, 1); s.run(1000);                            // the switch reads pressed at 40 %
  const a1 = s.cmd('ALARM?');
  s.cmd('VALVE SET 6', 90000);
  const at6 = s.cmd('VALVE?'), a6 = s.cmd('ALARM?');
  s.cmd('VALVE SET 2', 30000);
  const at5 = s.cmd('VALVE?'), ev = events(s, 5);
  check('pass band: no W-4 at 40 %, closes to 6 % past the pressed switch, stops at 5 %', on && a1 === 'ALARM NONE\r\n' &&
    /VALVE 6\.0/.test(at6) && a6 === 'ALARM NONE\r\n' && /VALVE (4\.9|5\.0)/.test(at5) && /W-1/.test(ev),
    [on, a1.trim(), at6.trim(), a6.trim(), at5.trim(), ev.slice(-300)].join(' | '));
}
{ // The Disable switch edges are saved settings (0.16.21, Ryan 12 Nov: "make the boundary editable"): From 10 from
  // its card's keypad and Save settings, and closing past the pressed switch then stops at 10 %. From at or above
  // To is refused and nothing is saved.
  const s = homed(load()); s.cmd('VALVE SET 40', 150000);
  s.tap(415, 184); s.tap(84, 128); s.tap(305, 72); s.tap(180, 128);   // Menu > Motor settings > Override, Disable switch on
  s.tap(65, 184); s.tap(40, 239); s.tap(230, 183); s.tap(320, 212);   // From: 1 0, Set
  shot(s, 'override-from-draft');
  s.tap(415, 128); s.tap(319, 138);                                    // Save settings, Save
  const saved = s.setting(CFG.PASS_LO);
  s.set(7, 1); s.run(1000);                                            // the switch reads pressed at 40 %
  s.cmd('VALVE SET 2', 120000);
  const at = s.cmd('VALVE?'), ev = events(s, 5);
  s.set(7, 0); s.run(1000);
  s.tap(415, 184); s.tap(84, 128); s.tap(305, 72);
  s.tap(65, 184); s.tap(170, 183); s.tap(230, 183); s.tap(320, 212);  // From: 6 0, above To 50
  s.tap(415, 128); s.tap(319, 138);
  const kept = s.setting(CFG.PASS_LO);
  check('Disable switch: From saved as 10 stops closing at 10 %; From over To refused', saved === 10 &&
    /VALVE (9\.9|10\.0)/.test(at) && /W-1/.test(ev) && kept === 10,
    [saved, at.trim(), kept, ev.slice(-200)].join(' | '));
}
{ // The bench cryostat (the page's flow line, fitted to the bench logs): AUTO holds 16 mbar with
  // bench gains, a relay there cycles at about the bench's period, and opening sticks at low openings.
  const s = load(); s.boot(undefined, { plant: 0 }); s.set(5, 40000); s.run(2000); s.home();
  const num = r => parseFloat((r.match(/-?[\d.]+/) || ['NaN'])[0]);
  allSets(s, 0.40, 0.75); s.cmd('SETPOINT 16'); s.cmd('MODE AUTO');
  s.run(540000);
  // Held: no swing over the last minute of 10 min. It may still creep a
  // little as the heat exchanger settles (the knee), as on the bench.
  const ps = [];
  for (let i = 0; i < 12; i++) { s.run(5000); ps.push(num(s.cmd('PRESSURE?'))); }
  const p16 = ps[ps.length - 1], x0 = num(s.cmd('VALVE?'));
  check('bench cryostat: AUTO holds 16 mbar (KP 0.40, TI 0.75)', s.reg() >= 1 && ps.every(p => Math.abs(p - 16) < 0.3),
    'regulation ' + s.reg() + ', ' + Math.min(...ps) + ' to ' + Math.max(...ps) + ' mbar, valve ' + x0 + ' %');
  s.cmd('MODE MANUAL');
  let hi = null, last = 0; const ups = [];
  for (let t = 0; t < 300; t++) {
    s.run(1000); const p = num(s.cmd('PRESSURE?', 50));
    const want = p < 15.84 ? true : p > 16.16 ? false : hi === null ? p < 16 : hi;   // starts on the sign of the error
    if (want !== hi) { hi = want; s.cmd('VALVE SET ' + (x0 + (hi ? 0.8 : -0.8)).toFixed(1), 50); if (hi && t > 60) ups.push(t); }
  }
  const pu = ups.length > 2 ? (ups[ups.length - 1] - ups[0]) / (ups.length - 1) : 0;
  check('bench cryostat: relay at 16 mbar cycles in 12 to 30 s (bench 18 s)', pu >= 12 && pu <= 30, 'period ' + pu + ' s');
  s.cmd('VALVE SET 3.0', 30000); s.run(120000);
  const p0 = num(s.cmd('PRESSURE?')); s.cmd('VALVE SET 3.2', 10000); s.run(20000);
  const pOpen = num(s.cmd('PRESSURE?')); s.cmd('VALVE SET 3.0', 10000); s.run(20000);
  const pBack = num(s.cmd('PRESSURE?'));
  check('bench cryostat: 0.2 % opening sticks, closing follows', Math.abs(pOpen - p0) < 0.1 * p0 + 0.05,
    [p0, pOpen, pBack].join(' -> '));
}
{ // The bench cryostat, 10 Nov: at 10 mbar for half an hour (heat exchanger cold), then SP 1. With
  // KP 1.20 (1 mbar's gain, found with the heat exchanger warm) AUTO swings over the knee as on the
  // bench (there 1 to 10 mbar, about 18 s a cycle); with KP 0.10 it comes down without swinging.
  const num = r => parseFloat((r.match(/-?[\d.]+/) || ['NaN'])[0]);
  const reach = kp => {
    const s = load(); s.boot(undefined, { plant: 0 }); s.set(5, 40000); s.run(2000); s.home();
    allSets(s, 0.40, 0.75); s.cmd('SETPOINT 10'); s.cmd('MODE AUTO'); s.run(1800000);
    allSets(s, kp.toFixed(2), 0.42); s.cmd('SETPOINT 1');
    let low = false, swings = 0;   // times it came back up from under 1.5 to over 3.5 mbar
    for (let t = 0; t < 120; t++) {
      s.run(1000); const p = num(s.cmd('PRESSURE?', 20));
      if (p < 1.5) low = true;
      else if (p > 3.5 && low) { swings++; low = false; }
    }
    return swings;
  };
  const hi = reach(1.2), lo = reach(0.1);
  check('bench cryostat: 10 -> 1 mbar at KP 1.20 swings back over 3.5 mbar at least twice, at KP 0.10 never',
    hi >= 2 && lo === 0, 'KP 1.20 ' + hi + ' swings, KP 0.10 ' + lo);
}
console.log(failed ? failed + ' failed' : 'all passed');
process.exit(failed ? 1 : 0);
