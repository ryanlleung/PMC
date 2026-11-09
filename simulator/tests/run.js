// Bench scenarios against the firmware build (pmc-wasm.js), in node:
//   node simulator/tests/run.js [--png <dir>]
// Each case boots a new board and checks the firmware's replies. The board
// sends nothing unasked, so a case reads the event log (EVNT?). The text
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
const events = (s, n = 100) => s.cmd('EVNT? ' + n, 2000);
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
  const st = s.cmd('MODE?;VALV?;VALV MOVING?;ALRM?'), ev = events(s);
  check('boot NOT HOMED', st === 'MODE NOT HOMED;VALV NONE;VALV MOVING 0;ALRM NONE\r\n', st);
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
  s.cmd('VALV SET 30', 60000); s.run(400000);
  const p = s.cmd('PRES?').match(/PRES ([\d.]+) mbar/)[1], before = s.get(30);
  // The tiles and header follow within 0.15 s of the release (0.15.4; the 0.5 s refresh was too slow).
  const hdrPx = () => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[3 * 480 + 415];
  const press = (x, y) => { s.x.sim_touch(x, y, 1); s.run(120); s.x.sim_touch(x, y, 0); s.run(150); };
  const manualPx = hdrPx();
  press(93, 184);
  check('Start AUTO shows within 0.15 s', hdrPx() !== manualPx);
  s.run(850);
  const sp = s.cmd('PRES SP?'), shown = (Math.round(parseFloat(p) * 10) / 10).toFixed(1);
  check('Start AUTO starts AUTO at the shown pressure', s.cmd('MODE?') === 'MODE AUTO\r\n' &&
    sp === 'PRES SP ' + shown + ' mbar\r\n', p + ' ' + sp);
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
  const tiles = [[84, 72, 'Trend'], [240, 72, 'Events'], [396, 72, 'PID tuning'], [84, 128, 'Valve settings'],
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
  // Choose sensor is gone (one sensor per board): no CFG_SENSOR key (27), the sensor range is the 15 psia Druck's,
  // and the old Settings slot now opens Review and save.
  check('no sensor-in-use setting', s.setting(27) === -2147483648 && s.setting(CFG.DBC) === 8, s.setting(27) + ' ' + s.setting(CFG.DBC));
  check('setpoint limit is the 15 psia range', s.cmd('PRES SP 1035') === 'ERR 102 Outside 0 to 1034.2 mbar\r\n', s.cmd('PRES SP 1035'));
  menu(); s.tap(386, 184); check('old Settings slot opens Review and save', screen() === 'Review and save', screen());
}
{ // PID tile opens PID tuning; KD is a saved setting from its keypad.
  const s = homed(load());
  shot(s, 'home-pid');
  s.tap(415, 72); shot(s, 'pid-tuning');
  s.tap(298, 156); s.tap(40, 239); s.tap(250, 239); s.tap(110, 183); s.tap(320, 212);   // KD 1.5, Set
  s.tap(415, 128); s.tap(319, 138);   // Save settings, then Save in the question
  check('KD saves from the PID tuning page', s.setting(CFG.KD) === 150, 'KD ' + s.setting(CFG.KD));
  // Gain schedule on (0.15.15): the PID tab shows the set row; Set 3's KP from its card.
  s.cmd('SCHED SET PV,6,20,15', 1000); s.run(1000);
  shot(s, 'pid-tuning-sets');
  s.tap(294, 128); s.tap(62, 184); s.tap(40, 239); s.tap(250, 239); s.tap(110, 183); s.tap(320, 212);   // Set 3, KP 1.5, Set
  s.tap(415, 128); s.tap(319, 138);
  check('Set 3 KP saves from the set row', s.setting(CFG.KP3) === 150 && s.setting(CFG.KP) === 400 && s.setting(CFG.KP2) === 400,
    [s.setting(CFG.KP), s.setting(CFG.KP2), s.setting(CFG.KP3)].join());
  shot(s, 'pid-tuning-set3');
}
{ // A tap on anything greyed round a popup closes it, as Back (0.15.16, Ryan 5 Nov: "Make popups exitable when
  // tapping the greyed out areas"): the dimmed page, the greyed strip, the greyed header icons. A tap on the box does not.
  const s = homed(load());
  // The header's title, left of the remote light: Home's, a keypad's or another page's.
  const title = () => { const fb = new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272); let h = 0;
    for (let y = 4; y < 36; y++) for (let x = 96; x < 290; x++) h = (h * 31 + fb[y * 480 + x]) >>> 0; return h; };
  const home = title(), at = s.cmd('VALV?');
  s.tap(290, 184); const kp = s.popup();                           // Go to: the keypad
  s.tap(110, 183); s.tap(100, 70); const kpStays = s.popup();       // 5, then a tap on its title
  shot(s, 'popup-keypad');
  s.tap(415, 72); s.run(2000);                                      // the dimmed page right of it (the PID tile under it)
  check('keypad: stays on a tap on its box, closes on the dimmed page; nothing set, still Home', kp && kpStays && !s.popup() &&
    s.cmd('VALV?') === at && !s.busy() && title() === home, [kp, kpStays, s.popup(), s.cmd('VALV?'), title() === home].join());
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
    pa && pb && !s.popup() && s.setting(CFG.SCHED) === 0, [pa, pb, s.popup(), s.setting(CFG.SCHED)].join());
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
    s.send('PRES?');
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
  const ok = s.setClosed(), st = s.cmd('MODE?;VALV?');
  check('set position by hand reads 0.00 %', ok && st === 'MODE MANUAL;VALV 0.00 %\r\n', s.why() + ' ' + st);
}
{ // Set as 0 % after homing reads 0.00 %, not the seat (Ryan, 4 Nov).
  const s = homed(load());
  s.cmd('VALV SET 10', 30000); s.cmd('STOP');
  const ok = s.setClosed(), st = s.cmd('VALV?');
  check('Set as 0 % after homing reads 0.00 %', ok && st === 'VALV 0.00 %\r\n', s.why() + ' ' + st);
}
{ // MANUAL go-to and the needle follows the coils.
  const s = homed(load());
  s.cmd('VALV SET 25', 60000);
  const full = s.get(5) / 2;
  // The needle is the count plus Return, since 0 % is above the switch.
  check('VALV SET 25 % moves the needle 25 % of 66048', Math.abs(s.get(30) - 16512) <= 1 && full - s.get(30) === s.setting(CFG.RETURN), 'needle ' + full + ', count ' + s.get(30));
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
  const s = homed(load());
  s.cmd('PRES SP 5'); s.cmd('MODE AUTO');
  s.run(400000);
  const d = s.cmd('PRES?;PRES SP?;MODE?;VALV?;ALRM?');
  check('AUTO 5 mbar stable', s.reg() === 3, 'regulation ' + s.reg());
  check('poll in AUTO', /^PRES [45]\.\d{3} mbar;PRES SP 5\.0 mbar;MODE AUTO;VALV \d+\.\d\d %;ALRM NONE\r\n$/.test(d), d);
  shot(s, 'auto');
  check('control tuning saves in AUTO', s.save(CFG.KP, 300) && s.setting(CFG.KP) === 300 &&
    s.cmd('MODE?') === 'MODE AUTO\r\n', s.why());
}
{ // A large setpoint change in AUTO applies at once from the SP keypad: no question (0.14.22).
  const s = homed(load());
  s.cmd('PRES SP 5;MODE AUTO'); s.run(5000);
  s.tap(121, 184); s.tap(40, 239); s.tap(110, 183); s.tap(320, 212);   // SP box, 1, 5, Set
  s.run(500);
  shot(s, 'sp-change');
  const st = s.cmd('PRES SP?;MODE?');
  check('SP 5 to 15 mbar in AUTO applies without a question', st === 'PRES SP 15.0 mbar;MODE AUTO\r\n', st);
}
{ // The motor supply is no longer sensed: counted moves can continue without physical motion.
  const s = homed(load());
  s.cmd('PRES SP 5;MODE AUTO'); s.run(400000);
  s.set(12, 0);
  const needle = s.get(5), count = s.get(30);
  s.cmd('PRES SP 8'); s.run(5000);
  check('motor power loss does not directly end AUTO', s.cmd('MODE?;ALRM?') === 'MODE AUTO;ALRM NONE\r\n');
  check('unpowered motor stays still while the step count advances', s.get(5) === needle && s.get(30) !== count);
}
{ // F-6: the needle slips in AUTO, so the pressure does not follow.
  const s = homed(load());
  s.cmd('PRES SP 5'); s.cmd('MODE AUTO'); s.run(300000);
  s.set(6, 0); s.cmd('PRES SP 8');
  s.run(600000);
  const ev = events(s, 10);
  check('F-6 no response', /F-6/.test(ev), ev);
  shot(s, 'f06');
  // Fault detail: its buttons sit in the strip's row, so the strip hides (0.14.13).
  s.tap(415, 184); s.tap(240, 72); s.tap(180, 72);   // Menu > Events (0.16.2), the fault
  shot(s, 'f06-detail');
  s.tap(181, 240);   // Reset fault
  check('fault detail Reset fault works', s.cmd('ALRM?') === 'ALRM NONE\r\n', s.cmd('ALRM?'));
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
  s.cmd('VALV SET 10', 20000); const saved = s.save(CFG.KP, 300); s.run(5000);
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
  s.tap(415, 184); s.tap(240, 128); s.tap(415, 184); s.tap(318, 147);   // Menu > Valve position (middle from 0.16.3) > Motor override > Open
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
  // No room sensor: the air point needs a typed reading, BARO? says NONE.
  const u = load(); u.boot(); u.set(20, 0); u.run(3000); u.set(11, 1); u.run(3000);
  check('BARO NONE without the sensor', u.cmd('BARO?', 1000) === 'BARO NONE\r\n', u.cmd('BARO?', 1000));
  const r = u.cal(2, -1, 2000);
  check('room air point refused without the sensor', !r && /no room pressure sensor/i.test(u.why()), u.why());
  check('typed air point still works', u.cal(2, 1013), u.why());
}
{ // The remote set: the reply rules of the handover page.
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
  s.home(200);
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
{ // Remote control switch: On by default; Off, the link takes queries only; On from the screen, it takes sets.
  const d = load(); d.boot(); d.run(1000);   // boot() leaves the switch as the firmware starts it
  check('switch On by default', d.cmd('PRES SP 5;PRES SP?') === 'PRES SP 5.0 mbar\r\n', d.cmd('PRES SP?'));
  const s = load(); s.boot(undefined, { remote: false }); s.set(5, 40000); s.run(2000);
  check('switch Off: PRES SP refused', s.cmd('PRES SP 5') === 'ERR 211 Read only over the network\r\n', s.cmd('PRES SP 5'));
  check('switch Off: STOP refused', s.cmd('STOP') === 'ERR 211 Read only over the network\r\n');
  check('switch Off: queries answer', s.cmd('MODE?;PRES SP?') === 'MODE NOT HOMED;PRES SP NONE\r\n', s.cmd('MODE?;PRES SP?'));
  s.tap(415, 184); s.tap(84, 184); s.tap(357, 128);   // Menu, System (0.16.2: bottom left), Remote control
  check('switch On from the screen: PRES SP taken', s.cmd('PRES SP 5;PRES SP?') === 'PRES SP 5.0 mbar\r\n', s.cmd('PRES SP?'));
  check('switch On is a NET event', /NET,Remote control on \(screen\)/.test(events(s, 5)), events(s, 5));
  shot(s, 'system');
  s.tap(357, 128);
  check('switch Off again: refused', s.cmd('PRES SP 6') === 'ERR 211 Read only over the network\r\n');
  check('switch Off: PID SET refused, PID? answers', s.cmd('PID SET 3,0.5') === 'ERR 211 Read only over the network\r\n' &&
    s.cmd('PID?') === 'PID 4.00,0.25,0.00\r\n', s.cmd('PID?'));
  check('switch Off: PIDS SET and SCHED SET refused, PIDS? and SCHED? answer',
    s.cmd('PIDS SET 2,3,0.5') === 'ERR 211 Read only over the network\r\n' &&
    s.cmd('SCHED SET PV,6,20,15') === 'ERR 211 Read only over the network\r\n' &&
    s.cmd('PIDS?;SCHED?') === 'PIDS 1,4.00,0.25,0.00,4.00,0.25,0.00,4.00,0.25,0.00;SCHED OFF,6.0,20.0,15\r\n', s.cmd('PIDS?;SCHED?'));
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
  check('start: OFF', s.cmd('MODE?;VALV?') === 'MODE OFF;VALV NONE\r\n', s.cmd('MODE?;VALV?'));
  check('start: black screen', px(s, 10, 10) === 0 && px(s, 240, 136) === 0, px(s, 10, 10));
  check('start: moves refused', s.cmd('VALV SET 10') === 'ERR 213 Controller is off\r\n', s.cmd('VALV SET 10'));
  check('start: BOOT event says OFF', /BOOT,Power-up \(event only\), OFF, valve not homed/.test(events(s, 5)), events(s, 5));
  shot(s, 'boot-off');
  s.tap(240, 136, 100); s.tap(240, 136, 150);
  check('turning on: the logo on white', px(s, 10, 10) === 0xFFFF, px(s, 10, 10));
  shot(s, 'turning-on');
  s.run(3000);
  check('double tap at start: NOT HOMED', s.cmd('MODE?') === 'MODE NOT HOMED\r\n', s.cmd('MODE?'));
  shot(s, 'turned-on');
}
{ // From the screen: the power icon (top left), Shut down, the black screen, OFF, a double tap.
  const s = homed(load());
  s.cmd('VALV SET 30', 60000); s.run(2000);
  s.tap(24, 20); shot(s, 'shutdown-ask');
  s.tap(318, 165, 3000);
  const during = s.cmd('MODE?;VALV MOVING?');
  check('shut down: to 0 %', during === 'MODE SHUTDOWN;VALV MOVING 1\r\n', during);
  check('shut down: ERR 213 while moving', s.cmd('VALV SET 10') === 'ERR 213 Shutting down, STOP to cancel\r\n' &&
    s.cmd('MODE MANUAL') === 'ERR 213 Shutting down, STOP to cancel\r\n', s.cmd('VALV SET 10'));
  shot(s, 'shutting-down');
  s.run(60000);
  const st = s.cmd('MODE?;VALV?;VALV MOVING?');
  check('shut down: OFF at 0 %', st === 'MODE OFF;VALV 0.00 %;VALV MOVING 0\r\n', st);
  check('shut down: needle at 0 %, switch not reached, coils off', s.get(5) === NEEDLE0 && s.get(15) === 0,
    'needle ' + s.get(5) + ' coils ' + s.get(15));
  check('OFF: ERR 213', s.cmd('VALV SET 10') === 'ERR 213 Controller is off\r\n' && s.cmd('MODE AUTO') === 'ERR 213 Controller is off\r\n' &&
    s.cmd('MODE MANUAL') === 'ERR 213 Controller is off\r\n', s.cmd('MODE AUTO'));
  check('OFF: pressure still read', /^PRES [\d.]+ mbar/.test(s.cmd('PRES?')), s.cmd('PRES?'));
  check('shut down event', /OFF,Valve at 0 %, count 0; coils off, OFF/.test(events(s, 5)), events(s, 5));
  shot(s, 'off');
  parkedFlash = s.flash();
  s.tap(240, 136, 1500);
  check('OFF: one tap does nothing', s.cmd('MODE?') === 'MODE OFF\r\n', s.cmd('MODE?'));
  s.tap(240, 136, 100); s.tap(240, 136, 300);
  s.run(3000);
  check('OFF: a double tap turns it on, MANUAL', s.cmd('MODE?;VALV?') === 'MODE MANUAL;VALV 0.00 %\r\n', s.cmd('MODE?;VALV?'));
  shot(s, 'on-again');
  s.cmd('VALV SET 5', 15000);
  check('on again: moves', s.cmd('VALV?') === 'VALV 5.00 %\r\n', s.cmd('VALV?'));
}
{ // Close the valve unticked (0.15.7): OFF at once, the valve left open, its count kept.
  const s = homed(load());
  s.cmd('VALV SET 30', 60000); s.run(2000);
  const needle = s.get(5);
  s.tap(24, 20); s.tap(240, 111); shot(s, 'shutdown-leave');
  s.tap(318, 165, 1000);
  check('shut down, valve left: OFF at 30 %', s.cmd('MODE?;VALV?;VALV MOVING?') === 'MODE OFF;VALV 30.00 %;VALV MOVING 0\r\n' &&
    s.get(5) === needle && s.get(15) === 0, s.cmd('MODE?;VALV?') + ' needle ' + s.get(5) + ' coils ' + s.get(15));
  check('shut down, valve left: event', /OFF,Shut down, valve left at 30\.0 %: coils off, OFF/.test(events(s, 5)), events(s, 5));
  const t = load(); t.boot(s.flash(), { off: true }); t.set(5, needle); t.run(2000);
  check('shut down, valve left: kept at the next start', t.cmd('MODE?;VALV?') === 'MODE OFF;VALV 30.00 %\r\n', t.cmd('MODE?;VALV?'));
  const u = homed(load());
  u.tap(24, 20); u.tap(240, 111); u.tap(240, 111); u.tap(318, 165, 1000);
  check('ticked again: closes', u.cmd('MODE?') === 'MODE OFF\r\n' && /at 0 %: coils off, OFF/.test(events(u, 5)), events(u, 5));
}
{ // Power-up after Shut down, the switch released: OFF, the count kept; on, it moves at once.
  for (const r of [0, 1, 2]) {
    const s = load(); s.boot(parkedFlash, { off: true, reset: r }); s.set(5, NEEDLE0); s.run(2000);
    const what = ['power-up', 'watchdog', 'Restart'][r];
    check('start after Shut down (' + what + '): OFF at 0 %', s.cmd('MODE?;VALV?') === 'MODE OFF;VALV 0.00 %\r\n' &&
      s.offer() === -2147483648, s.cmd('MODE?;VALV?') + ' offer ' + s.offer());
    if (r) continue;
    check('start after Shut down: event', /REF,Kept from the last shut down: count 0 \(0\.0 %\)\r?\n/.test(events(s, 5)), events(s, 5));
    s.turnOn();
    s.cmd('VALV SET 20', 60000);
    check('start after Shut down: on, moves at once', s.cmd('MODE?;VALV?') === 'MODE MANUAL;VALV 20.00 %\r\n' &&
      s.get(5) / 2 === 400 + Math.round(66048 * 0.2), s.cmd('MODE?;VALV?') + ' needle ' + s.get(5));
  }
}
{ // The valve closed onto the switch while the power was off: not homed, nothing offered.
  const s = load(); s.boot(parkedFlash, { off: true }); s.set(5, 0); s.run(2000);
  check('kept but switch pressed: not homed', s.cmd('MODE?;VALV?') === 'MODE OFF;VALV NONE\r\n' && s.offer() === -2147483648,
    s.cmd('MODE?;VALV?') + ' offer ' + s.offer());
  check('kept but switch pressed: event', /limit switch is pressed: home the valve/.test(events(s, 5)), events(s, 5));
  s.turnOn();
  check('kept but switch pressed: on, NOT HOMED', s.cmd('MODE?') === 'MODE NOT HOMED\r\n', s.cmd('MODE?'));
  const t = load(); t.boot(s.flash(), { off: true }); t.set(5, NEEDLE0); t.run(2000);
  check('kept entry withdrawn', t.cmd('MODE?;VALV?') === 'MODE OFF;VALV NONE\r\n' && t.offer() === -2147483648,
    t.cmd('MODE?;VALV?') + ' offer ' + t.offer());
}
{ // Set by hand, not homed: kept through Shut down, but still not homed (SAVED).
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  s.setClosed();
  check('set by hand at 0 %: OFF at once', s.shutdown(300) && s.cmd('MODE?;VALV?') === 'MODE OFF;VALV 0.00 %\r\n', s.why() + s.cmd('MODE?;VALV?'));
  const t = load(); t.boot(s.flash(), { off: true }); t.set(5, 40000); t.run(2000);
  check('set by hand: kept, not homed', t.cmd('VALV?') === 'VALV 0.00 %\r\n' &&
    /Kept from the last shut down: count 0 \(0\.0 %\), not homed/.test(events(t, 5)), t.cmd('VALV?') + events(t, 5));
}
{ // Switched off without Shut down: the saved position offered, and the log says so.
  const s = homed(load());
  s.cmd('VALV SET 30', 60000); s.run(3000);
  const t = load(); t.boot(s.flash()); t.set(5, 40000); t.run(2000);
  check('switched off without Shut down', t.offer() === Math.round(66048 * 0.3) && /Last power-off without Shut down/.test(events(t, 5)),
    t.offer() + '\n' + events(t, 5));
  shot(t, 'unparked-ask');
}
{ // STOP cancels: MANUAL, the valve where it stopped.
  const s = homed(load());
  s.cmd('VALV SET 50', 90000); s.run(2000);
  s.shutdown(5000);
  s.cmd('STOP', 1000);
  const v = parseFloat(s.cmd('VALV?').match(/VALV ([-\d.]+)/)[1]);
  check('STOP cancels shut down: MANUAL', s.cmd('MODE?') === 'MODE MANUAL\r\n' && v > 40 && v < 48 && !s.busy(), s.cmd('MODE?;VALV?'));
  check('STOP cancels shut down: event', /shut down cancelled/.test(events(s, 5)), events(s, 5));
}
{ // From AUTO: AUTO ends, then the same move to 0 %.
  const s = homed(load());
  s.cmd('VALV SET 30', 60000);
  s.cmd('PRES SP 3;MODE AUTO', 20000);
  check('in AUTO before shut down', s.cmd('MODE?') === 'MODE AUTO\r\n', s.cmd('MODE?'));
  check('shut down from AUTO', s.shutdown(1000) && s.cmd('MODE?') === 'MODE SHUTDOWN\r\n', s.why() + s.cmd('MODE?'));
  s.run(90000);
  check('shut down from AUTO: OFF at 0 %', s.cmd('MODE?;VALV?') === 'MODE OFF;VALV 0.00 %\r\n', s.cmd('MODE?;VALV?'));
}
{ // Not homed: OFF at once, the valve not moved.
  const s = load(); s.boot(); s.set(5, 40000); s.run(2000);
  check('shut down not homed: OFF, not moved', s.shutdown(2000) && s.cmd('MODE?;VALV?') === 'MODE OFF;VALV NONE\r\n' &&
    s.get(5) === 40000, s.why() + s.cmd('MODE?;VALV?') + ' needle ' + s.get(5));
  check('shut down not homed: event', /OFF,Shut down, not homed: valve not moved/.test(events(s, 5)), events(s, 5));
}
{ // The switch before 0 % (ice, an early switch): MANUAL there and the switch question.
  const s = homed(load());
  s.cmd('VALV SET 30', 60000);
  s.set(18, 4000); s.set(7, 3);   // trips 2000 full steps above the seat
  s.shutdown(60000);
  const st = s.cmd('MODE?;VALV?');
  check('early switch: MANUAL, count kept', /^MODE MANUAL;VALV 2\.\d\d %/.test(st), st);
  check('early switch: event and question', /Shut down stopped \(limit switch pressed\)/.test(events(s, 5)) &&
    /W-1,Limit switch pressed/.test(events(s, 5)), events(s, 5));
  shot(s, 'early-switch');
  // The switch question needs an answer: a tap round it or on the strip leaves it (0.15.16).
  const asked = s.popup();
  s.tap(455, 100); s.tap(100, 240);
  check('switch question stays on a tap round it', asked && s.popup() && /W-1/.test(s.cmd('ALRM?')), [asked, s.popup(), s.cmd('ALRM?')].join());
}
{ // Already at 0 %: OFF at once.
  const s = homed(load());
  check('at 0 %: OFF at once', s.shutdown(300) && s.cmd('MODE?;VALV?') === 'MODE OFF;VALV 0.00 %\r\n', s.why() + s.cmd('MODE?;VALV?'));
}
{ // PID? and PID SET (0.15.8): the PID tab's Save over the text port, also in AUTO.
  const s = homed(load());
  const one = c => s.cmd(c, 1000);
  check('PID? defaults', one('PID?') === 'PID 4.00,0.25,0.00\r\n', one('PID?'));
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
  s.cmd('VALV SET 30', 60000);
  s.cmd('PRES SP 3;MODE AUTO', 20000);
  check('PID SET in AUTO, AUTO kept', one('PID SET 2,0.4;PID?;MODE?') === 'PID 2.00,0.40,1.25;MODE AUTO\r\n', one('PID?;MODE?'));
  check('PID SET in AUTO saves', s.setting(CFG.KP) === 200 && s.setting(CFG.TI) === 40, s.setting(CFG.KP) + ' ' + s.setting(CFG.TI));
}
{ // Gain schedule (0.15.15, Ryan 5 Nov "build like industry ones"): PIDS?, PIDS SET, SCHED?, SCHED SET,
  // PID? and PID SET on the set in use, and AUTO switching sets on the pressure with hysteresis.
  const s = homed(load());
  const one = c => s.cmd(c, 1000);
  check('PIDS? defaults: three sets as set 1', one('PIDS?') === 'PIDS 1,4.00,0.25,0.00,4.00,0.25,0.00,4.00,0.25,0.00\r\n', one('PIDS?'));
  check('SCHED? defaults: Off, 6 and 20 mbar, 15 %', one('SCHED?') === 'SCHED OFF,6.0,20.0,15\r\n', one('SCHED?'));
  check('PIDS SET silent, PIDS? confirms', one('PIDS SET 2,5,0.3') === '' && one('PIDS SET 3, 6, 0.35, 0.5') === '' &&
    one('PIDS?') === 'PIDS 1,4.00,0.25,0.00,5.00,0.30,0.00,6.00,0.35,0.50\r\n', one('PIDS?'));
  check('PIDS SET saves the settings', s.setting(CFG.KP2) === 500 && s.setting(CFG.TI2) === 30 && s.setting(CFG.KD3) === 50,
    [s.setting(CFG.KP2), s.setting(CFG.TI2), s.setting(CFG.KD3)].join());
  const bad = [['PIDS SET', 101], ['PIDS SET 2', 101], ['PIDS SET 2,1', 101], ['PIDS SET x,1,1', 101], ['PIDS SET 2,1,1,1,1', 101],
    ['PIDS SET 0,1,1', 102], ['PIDS SET 4,1,1', 102], ['PIDS SET 2,0,1', 102], ['PIDS SET 3,1,-1', 102],
    ['SCHED SET', 101], ['SCHED SET ON,6,20,15', 101], ['SCHED SET PV,6,20', 101], ['SCHED SET PV,6,20,15,1', 101],
    ['SCHED SET PV,x,20,15', 101], ['SCHED SET PV,0,20,15', 102], ['SCHED SET PV,6,20,41', 102], ['SCHED SET PV,20,6,15', 102],
    ['SCHED SET PV,10,11,15', 102]];
  const wrong = bad.map(([c, n]) => [c, n, one(c)]).filter(([c, n, r]) => !new RegExp('^ERR ' + n + ' [A-Z][^;\\r\\n]*\\r\\n$').test(r))
    .map(([c, n, r]) => c + ' -> ' + JSON.stringify(r) + ' (want ' + n + ')');
  check('PIDS SET and SCHED SET refusals', wrong.length === 0, wrong.join('\n     '));
  check('SCHED SET boundaries text', one('SCHED SET PV,20,6,15') === 'ERR 102 B2 must be above B1, by more than the hysteresis either side\r\n',
    one('SCHED SET PV,20,6,15'));
  check('refusals change nothing', one('SCHED?;PIDS?') === 'SCHED OFF,6.0,20.0,15;PIDS 1,4.00,0.25,0.00,5.00,0.30,0.00,6.00,0.35,0.50\r\n',
    one('SCHED?;PIDS?'));
  check('SCHED SET PV', one('SCHED SET PV,6,20,15') === '' && one('SCHED?') === 'SCHED PV,6.0,20.0,15\r\n' &&
    s.setting(CFG.SCHED) === 2 && s.setting(CFG.B1) === 60 && s.setting(CFG.B2) === 200, one('SCHED?'));
  // The sim plant runs about 0.8 to 12.5 mbar (20 to 100 % open), so the
  // boundaries here are 3 and 8 mbar: up at 3.45 and 9.2, down at 6.8 and 2.55.
  // Sim-plant gains, a different set each so PID? shows which is in use.
  one('SCHED SET PV,3,8,15;PIDS SET 1,6,0.25;PIDS SET 2,5,0.25,0;PIDS SET 3,4,0.25,0');
  // The set changes so far, oldest first (EVNT? lists the newest first).
  const sws = () => [...events(s, 100).matchAll(/AUTO PID set (\d) to (\d), pressure ([\d.]+) mbar/g)].map(m => [+m[1], +m[2], +m[3]]).reverse();
  s.cmd('VALV SET 40', 60000);
  s.run(240000);
  s.cmd('PRES SP 10;MODE AUTO', 1000);
  check('AUTO starts on the set for the pressure (3.8 mbar: set 2)', /Auto at setpoint 10\.0 mbar, valve [\d.]+ %, PID set 2/.test(events(s, 3)),
    events(s, 3));
  s.run(600000);
  check('PV schedule: set 3 at 10 mbar', /^PIDS 3,/.test(one('PIDS?')) && one('PID?') === 'PID 4.00,0.25,0.00\r\n',
    one('PRES?;PIDS?;PID?'));
  const first = sws();
  check('then 2 to 3 over 9.2 mbar (B2 x 1.15), once', first.length === 1 && first[0][0] === 2 && first[0][1] === 3 && first[0][2] >= 9.2 &&
    first[0][2] < 10.5, JSON.stringify(first));
  // PID SET edits the set in use.
  check('PID SET edits the set in use', one('PID SET 4.5,0.25;PID?') === 'PID 4.50,0.25,0.00\r\n' && s.setting(CFG.KP3) === 450 &&
    s.setting(CFG.KP) === 600, s.setting(CFG.KP3) + ' ' + s.setting(CFG.KP));
  s.cmd('PRES SP 2', 1000);
  s.run(900000);
  const sw = sws().slice(1);
  check('10 -> 2: set 3 to 2 under 6.8 mbar (B2 x 0.85)', sw.length >= 1 && sw[0][0] === 3 && sw[0][1] === 2 && sw[0][2] <= 6.8 &&
    sw[0][2] > 5.5, JSON.stringify(sw));
  check('10 -> 2: then set 2 to 1 under 2.55 mbar (B1 x 0.85), no other change', sw.length === 2 && sw[1][0] === 2 && sw[1][1] === 1 &&
    sw[1][2] <= 2.6 && sw[1][2] > 2.0, JSON.stringify(sw));
  check('10 -> 2: AUTO kept, near 2 mbar, set 1', one('MODE?') === 'MODE AUTO\r\n' && Math.abs(parseFloat(one('PRES?').slice(5)) - 2) < 0.3 &&
    /^PIDS 1,/.test(one('PIDS?')) && one('PID?') === 'PID 6.00,0.25,0.00\r\n', one('MODE?;PRES?;PIDS?;PID?'));
  s.cmd('PRES SP 10', 1000);
  s.run(900000);
  const up = sws().slice(3);
  check('2 -> 10: set 1 to 2 over 3.45, set 2 to 3 over 9.2 mbar', up.length === 2 && up[0][0] === 1 && up[0][1] === 2 &&
    up[0][2] >= 3.4 && up[0][2] < 4.5 && up[1][0] === 2 && up[1][1] === 3 && up[1][2] >= 9.2 && up[1][2] < 10.5, JSON.stringify(up));
  shot(s, 'gain-schedule');
  // The setpoint schedule is gone (0.15.20, Ryan 6 Nov): SP is refused, the table stays on the pressure.
  check('SCHED SET SP refused', one('SCHED SET SP,3,8,15') === 'ERR 101 Expected: SCHED SET <OFF|PV>,<b1>,<b2>,<h>\r\n' &&
    one('SCHED?') === 'SCHED PV,3.0,8.0,15\r\n', one('SCHED SET SP,3,8,15') + one('SCHED?'));
  check('SCHED SET OFF: set 1', one('SCHED SET OFF,6,20,15') === '' && (s.run(3000), /^PIDS 1,/.test(one('PIDS?'))) &&
    one('PID?') === 'PID 6.00,0.25,0.00\r\n', one('PIDS?;PID?'));
}
{ // The limit switch pressed away from the seat (0.16.4, Ryan PR #43): a yellow edge round Home's Valve tile at
  // once (the "Limit" tag of 0.16.1 gone) and W-4 within 0.3 s (two 1 s reads to 0.16.3); released, the edge goes.
  const s = homed(load()); s.cmd('VALV SET 20', 60000);
  const px = (x, y) => new Uint16Array(s.x.memory.buffer, s.x.sim_fb(), 480 * 272)[y * 480 + x];
  const before = px(187, 100);
  s.set(7, 1); s.run(200);   // the wire open: the NC contact reads pressed
  const edge = px(187, 100), al = s.cmd('ALRM?', 100);
  shot(s, 'limit-edge');
  s.set(7, 0); s.run(300);
  const after = px(187, 100);
  check('limit pressed: yellow edge on the Valve tile, W-4 within 0.3 s', before === 0xFFFF && edge === 0xF606 && /W-4/.test(al) &&
    after === 0xFFFF, [before.toString(16), edge.toString(16), al.trim(), after.toString(16)].join(' '));
}
{ // Switch pass band (0.16.5, Ryan PR #43: the force switch trips about 13 % where the valve binds): Motor override >
  // Switch turns it on; between 10 and 20 % the pressed switch neither stops closing nor raises W-4; below 10 % it
  // stops as always.
  const s = homed(load()); s.cmd('VALV SET 15', 60000);
  s.tap(415, 184); s.tap(240, 128); s.tap(415, 184); s.tap(318, 147);   // Menu > Valve position > Motor override > Open
  s.tap(203, 72); s.tap(120, 128);                     // Switch tab, Pass 10 to 20 % on
  shot(s, 'motor-switch');
  const on = /Switch passed between 10 and 20 % open: on/.test(events(s, 5));
  s.set(7, 1); s.run(1000);                            // the switch reads pressed at 15 %
  const a1 = s.cmd('ALRM?');
  s.cmd('VALV SET 11', 30000);
  const at11 = s.cmd('VALV?');
  s.cmd('VALV SET 5', 30000);
  const at10 = s.cmd('VALV?'), ev = events(s, 5);
  check('pass band: no W-4 at 15 %, closes to 11 % past the pressed switch, stops at 10 %', on && a1 === 'ALRM NONE\r\n' &&
    /VALV 11\.0/.test(at11) && /VALV (9\.9|10\.0)/.test(at10) && /W-1/.test(ev),
    [on, a1.trim(), at11.trim(), at10.trim(), ev.slice(-300)].join(' | '));
}
console.log(failed ? failed + ' failed' : 'all passed');
process.exit(failed ? 1 : 0);
