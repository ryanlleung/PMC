// Text fit check: lists every label whose text does not fit its
// box, is cut by its card or button, or runs into the label or object
// beside it (simulator/wasm/textfit.c), on:
//  - every screen, tab, popup and keypad reached by tapping, breadth first,
//    from a few starting states (not homed, MANUAL, AUTO, faults, off) and
//    with the largest and smallest value of every setting;
//  - every message and question the firmware can show, from the source
//    (refusal reasons, notes, questions) and with long values filled in.
//   node simulator/tests/text-fit.js [--depth N] [--verbose]
// Exits 1 if anything does not fit.
const { load, CFG } = require('./sim-node.js');
const fs = require('fs'), path = require('path');
const argv = process.argv;
const maxDepth = argv.includes('--depth') ? +argv[argv.indexOf('--depth') + 1] : 6;
const verbose = argv.includes('--verbose');
const SRC = path.join(__dirname, '../../firmware');
const CFG_N = 32;

function cstr(s, p) {
  const m = new Uint8Array(s.x.memory.buffer); let e = p;
  while (m[e]) e++;
  return Buffer.from(m.subarray(p, e)).toString();
}
function put(s, text) {
  const b = Buffer.from(text + '\0');
  new Uint8Array(s.x.memory.buffer, s.x.sim_text_buf(), b.length).set(b);
}
const snap = s => new Uint8Array(s.x.memory.buffer).slice();
const restore = (s, m) => new Uint8Array(s.x.memory.buffer).set(m);

const found = new Map();   // problem -> the first place it showed
let states = 0, shown = 0;

function record(s, where) {
  for (const line of cstr(s, s.x.sim_textfit()).split('\n')) {
    if (!line) continue;
    // One problem on many screens, or with numbers that move (pressure,
    // time, the box's place), is one line: keep the first.
    const key = line.split(' | ').slice(1).join(' | ').replace(/(?<![A-Za-z-])\d+(\.\d+)?/g, '#');
    if (!found.has(key)) found.set(key, { line, where });
  }
}

// A keypad (its Set key, 71 x 96, at 309,209):
// the longest entry, then Set with it and with 0, for the field and the
// refusal lines.
const KEY_9 = [173, 131], KEY_0 = [237, 183], KEY_SET = [309, 209];
const keypads = new Set();
function keypad(s, where, clicks) {
  if (!clicks.includes(KEY_SET.join(',') + ',71,96')) return;
  const id = cstr(s, s.x.sim_screen()) + '|' + cstr(s, s.x.sim_popup_text());
  if (keypads.has(id)) return;
  keypads.add(id);
  const m = snap(s);
  for (let i = 0; i < 12; i++) s.tap(...KEY_9, 200);
  record(s, where + ' > 999999999999');
  s.tap(...KEY_SET, 600);
  record(s, where + ' > 999999999999 > Set');
  restore(s, m);
  s.tap(...KEY_0, 200); s.tap(...KEY_SET, 600);
  record(s, where + ' > 0 > Set');
  restore(s, m);
}

// Breadth first, so each screen is opened by its shortest path; each path is
// replayed from the starting state (a copy of the memory), one tap at a time.
function crawl(s, root, maxStates = 3000) {
  const seen = new Set(), start = snap(s);
  let queue = [[]];
  for (let depth = 0; depth <= maxDepth && queue.length; depth++) {
    const next = [];
    for (const p of queue) {
      restore(s, start);
      for (const [x, y] of p) s.tap(x, y, 600);
      const where = root + (p.length ? ' > ' + p.map(([x, y, t]) => t + '@' + x + ',' + y).join(' > ') : '');
      record(s, where);
      const clicks = cstr(s, s.x.sim_clickables());
      const title = cstr(s, s.x.sim_screen());
      keypad(s, where, clicks);
      const sig = title + '|' + cstr(s, s.x.sim_popup_text()) + '|' + clicks;
      if (seen.has(sig) || seen.size >= maxStates) continue;
      seen.add(sig);
      states++;
      if (depth < maxDepth)
        for (const c of clicks.split(';').filter(Boolean)) {
          const [x, y] = c.split(',').map(Number);
          next.push(p.concat([[x, y, title]]));
        }
    }
    queue = next;
  }
  restore(s, start);
  if (verbose) console.log(root + ': ' + seen.size + ' screens');
}

// A board with the position set as closed, the valve at pct.
function manual(pct = 34) {
  const s = load(); s.boot(); s.run(2000); s.setClosed(); s.cmd('VALVE SET ' + pct, 1000);
  for (let t = 0; t < 600000 && s.busy(); t += 1000) s.run(1000);
  s.run(5000);
  return s;
}

const t0 = Date.now();

/* ---- Every screen, from each starting state ---------------------------- */
{ const s = load(); s.boot(); s.set(5, 40000); s.run(3000); crawl(s, 'not homed'); }
{ crawl(manual(), 'manual'); }
{ const s = manual(30); s.run(60000); s.tap(93, 184); s.run(5000); crawl(s, 'auto'); }
{ const s = manual(); s.set(8, 0); s.run(5000); crawl(s, 'F-1'); }
// F-2 (the switch never trips while homing) with F-1, so the detail has Next fault.
{ const s = load(); s.boot(); s.set(5, 40000); s.set(7, 2); s.run(2000); s.home(400000); s.set(8, 0); s.run(5000); crawl(s, 'F-2 and F-1'); }
// F-3: homed, then the switch stays pressed when homing moves off it.
{ const s = load(); s.boot(); s.set(5, 40000); s.run(2000); s.home(); s.set(7, 1); s.home(60000); crawl(s, 'F-3'); }
// The limit switch pressed away from the closed point: the W-1 question.
{ const s = manual(); s.set(7, 1); s.run(5000); crawl(s, 'W-1'); }
// A homed valve shut down and switched on again: Valve > Position's "Before Off" (the crawl does not
// reach this state by itself).
{ const a = load(); a.boot(); a.set(5, 40000); a.run(2000); a.home(); a.shutdown(60000);
  const s = load(); s.boot(a.flash()); s.set(5, 800); s.run(3000); crawl(s, 'after shut down'); }
// Power lost at 34 %: the start-up question offers the saved position.
{ const a = manual(); a.run(3000); const s = load(); s.boot(a.flash()); s.run(3000); crawl(s, 'start-up'); }
{ const s = load(); s.boot(null, { off: true }); s.run(3000); crawl(s, 'off'); }
// Diagnostics at the ends of its readings: signal and excitation high, a cold board.
{ const s = manual(); s.set(10, 14.999); s.set(12, 14.99); s.set(13, -40.5); s.set(19, 1099.9); s.run(5000); crawl(s, 'readings high'); }
// Air in the line, the valve fully open, a long address, a long name, every
// setting at the end of its range with the largest number (then the
// smallest), changed but not saved, so Save, Undo and the list show.
for (const end of [1, 0]) {
  // The address takes effect at a restart: set it, then start a board from that flash.
  const a = load(); a.boot(); a.run(2000);
  put(a, '200.200.200.200 255.255.255.0'); a.x.sim_net();
  put(a, 'WWWWWWWWWWWW'); a.x.sim_name();
  a.run(1000);
  const s = load(); s.boot(a.flash()); s.run(2000); s.setClosed(); s.cmd('VALVE SET 100', 1000);
  for (let t = 0; t < 600000 && s.busy(); t += 1000) s.run(1000);
  // Largest from the last key (B3 before B2, To before From), smallest from the first.
  let applied = 0;
  for (let i = 0; i < CFG_N; i++) { const k = end ? CFG_N - 1 - i : i; applied += s.x.sim_apply(k, s.x.sim_range(k, end)); }
  if (applied < 20) throw new Error('only ' + applied + ' settings applied: ' + cstr(s, s.x.sim_why()));
  s.run(2000);
  crawl(s, end ? 'largest settings' : 'smallest settings');
}

/* ---- Every message and question --------------------------------------- */
// String literals in the source: the refusal reasons (why = "...", and
// return "..." in the functions that give one), and the texts of the
// message, note and question calls.
const lits = { msg: new Set(), cfg: new Set(), cal: new Set(), ask: new Set() };
const literal = t => [...t.matchAll(/"((?:[^"\\]|\\.)*)"/g)].map(m => JSON.parse('"' + m[1].replace(/\\x([0-9A-Fa-f]{2})/g, '\\u00$1') + '"'));
const reason = t => /^[A-Za-z][^%\r]* [^%\r]*$/.test(t) && !/^[A-Z]{2,} /.test(t);
for (const f of fs.readdirSync(SRC).filter(f => /\.c$/.test(f) && !/^(remote|link|main|lv_|ui_font|ui_img)/.test(f))) {
  const src = fs.readFileSync(path.join(SRC, f), 'utf8');
  const bag = f === 'cfg.c' ? lits.cfg : f === 'cal.c' ? lits.cal : lits.msg;
  for (const m of src.matchAll(/why\s*=\s*("(?:[^"\\]|\\.)*")/g)) literal(m[1]).filter(reason).forEach(t => bag.add(t));
  for (const m of src.matchAll(/return\s+((?:"(?:[^"\\]|\\.)*"\s*)+);/g)) {
    const t = literal(m[1]).join('');
    if (reason(t)) bag.add(t);
  }
  // The calls: the statement up to its ";". A question: its text, not the button names.
  for (const m of src.matchAll(/\bui_(msg|note|msg_no_ok|confirm|ask3?|ask4)\(([^;]*);/g)) {
    const ask = /^(confirm|ask)/.test(m[1]);
    const ts = literal(m[2]);
    for (const t of ask ? ts.slice(0, 1) : ts) if (t.length > 2 && !/%/.test(t)) (ask ? lits.ask : lits.msg).add(t);
  }
}
// The reasons as they are shown: on their own, after what was refused
// (settings: the reasons a save, undo, reset or switch can fail with; the
// band, address and name reasons come from a keypad or on their own), and
// as a keypad's refusal.
const reasons = [...lits.msg, ...lits.cal, ...lits.cfg];
// The PID zone refusals (ui_settings.c bound_why) with the widest values filled in; a keypad only.
const zoneWhy = ['Must be 999.9 mB or less:\nZone 4 starts at 1000.0 mB\n(40 % hysteresis).',
  'Must be 999.9 mB or more:\nZone 2 starts at 999.8 mB\n(40 % hysteresis).',
  '0 for no Zone 4, or 999.9 mB\nor more: Zone 3 starts at\n999.8 mB (40 % hysteresis).',
  'No Zone 4 fits: Zone 3\nstarts at 1000.0 mB\n(40 % hysteresis). Enter 0.', 'Must be 39 % or less: above that the zones overlap.',
  'Must be 20000 or more: Start is 20000 st/s', 'Must be 20000 or less: Speed is 20000 st/s', 'Must be below To (100 %)',
  'Must be above From (100 %)', 'Whole numbers only'];
// The band and Disable reasons come from a keypad only, the name's from the text port only.
const keyOnly = /^(B[23] |mask |not a device|that is the |name:|Disable:)/;
// The mask reason is the text port's: the screen says "Mask not valid".
const msgs = reasons.filter(w => !/^(B[23] |name:|Disable:|mask )/.test(w));
for (const w of lits.cfg) if (!keyOnly.test(w))
  for (const p of ['Not changed: ', 'Not saved: ', 'Not undone: ', 'Not reset: ', 'In use, not saved: ']) msgs.push(p + w);
for (const w of lits.cal) for (const p of ['Not started: ', 'Not saved: ']) msgs.push(p + w);
msgs.push('F-1, F-2, F-3 not reset: cause still present', 'F-2, F-3 reset. F-1 not reset: cause still present',
  'All settings saved (clock not set)', 'AUTO started at 1013.2 mB, the pressure now',
  'Homed in 9999 s. Switch at -100.00 % of the last homing', 'Homed in 9999 s',
  'Shut down cancelled. Valve at 100.00 %', 'Stopped. Capture cancelled', 'Stopped. AUTO ended',
  'Network: setpoint 1013.2 mB, not in use in MANUAL', 'Position now 100.00 %', 'Full travel now 99.99 turns',
  'All settings saved 16/11/26 23:59', 'Calibration saved 16/11/26 23:59', 'Network: address 200.200.200.200/24 set',
  'Not captured. Barometer 1013.2 mB', 'Capturing 10 of 10', '1013.25 mB at 12.345 mV/V',
  'Signal now -12.345 mV/V', 'Last saved 100.00 %');
// The status lines, at the width the strip gives them (ui.c strip_draw):
// 148 px beside the sparkline (the mode icon), 187 with the icon
// and OK or an action (184 inside a fault's edge), 299 with neither. Two
// lines fit the strip.
const statusLines = [
  [148, ['Stable at 1013.2 mB', 'In band, 1013.2 mB', 'Regulating to 1013.2 mB', 'AUTO at 1013.2 mB, no reading',
         'Moving to -100.00 %', 'Limit switch pressed at -100.00 %', 'On the switch', 'Position unverified', 'Valve idle']],
  [184, ['F-1 No pressure signal (+2)', 'F-2 Limit switch not found (+2)', 'F-3 Limit switch stays pressed (+2)']],
  [187, ['W-2 No pressure response: switched to MANUAL', 'Shut down stopped: limit switch pressed before 0 %',
         'AUTO stopped by W-1: limit switch pressed', 'AUTO stopped by F-1. Start AUTO to regulate again']],
  [299, ['Valve not homed', 'Homing: moving off the switch', 'Homing 1 of 4: fast close, up to 99 min', 'Homing 2 of 4: backing off',
         'Homing 3 of 4: slow close', 'Homing 4 of 4: return', '-300000 steps. Limit switch pressed: hold Limit override to close',
         '-300000 steps. Closing past the switch', '-300000 steps. Closing, limit switch overridden', '-300000 steps. Opening above 100 %',
         '-300000 steps. Valve idle', 'Limit override held: press Close or Open', '-999.99 turns. Valve idle',
         'Switch not read yet. Disabled 100 to 100 %', 'Full travel 999 min 59 s at this speed',
         'After restart: 200.200.200.200/24', 'Save keeps the address. It is in use after a restart.',
         'New calibration in use, not saved', 'Nominal calibration, nothing saved yet', 'Calibration saved 16/11/26 23:59',
         'Vacuum point captured. Not in use until the air point is captured', 'Air point captured. Not in use until the vacuum point is captured',
         '3 faults latched. Tap one for detail.', 'No faults latched. W-2 shown on Home', '32 unsaved changes', 'All settings saved 16/11/26 23:59',
         'Valve held at AUTO min 100.0 %', 'Valve at 100 %: 1013.2 mB not reached', 'Valve above 100 %',
         'W-1 Limit switch pressed at -100.00 %']],
];
const asks = [...lits.ask,
  'AUTO stopped: limit switch pressed at 100.00 %.\nSkip: keep the current position.\nSet as closed: this becomes 0 %.',
  'Power lost without shut down.\nValve was moving: last saved position 100.00 %, within ±100.0 %.\nHome valve closes the valve, up to 99 min.',
  'Power lost in AUTO at 1013.2 mB.\nValve was moving: last saved position 100.00 %, within ±100.0 %.\nHome valve closes the valve, up to 99 min.',
  'Restarted.\nLast saved position 100.00 %.\nHome valve closes the valve, up to 99 min.',
  'Home the valve? It closes onto the limit switch to find 0 %, up to 99 min.',
  'Valve position unknown. Home the valve (it closes, up to 99 min) or set the position?',
  'Set motor direction to Reverse? The valve must be homed again.', 'Undo 32 unsaved changes?', '32 unsaved changes. Save?',
  'Is the sensor vented to room air? Capture sets this point to 1013.2 mB.'];
{
  const s = manual(), start = snap(s);
  for (const [where, nav] of [['Home', []], ['Motor', [[415, 184], [84, 128]]], ['Events', [[415, 184], [240, 72]]]]) {
    restore(s, start);
    for (const [x, y] of nav) s.tap(x, y, 600);
    const page = snap(s);
    for (const t of msgs) for (const kind of [0, 1]) {
      restore(s, page); put(s, t); s.x.sim_show(kind); s.run(600);
      record(s, where + ', ' + (kind ? 'note' : 'message') + ': ' + t); shown++;
    }
    if (where !== 'Home') continue;
    for (const t of asks) for (const kind of [2, 3]) {
      restore(s, page); put(s, t); s.x.sim_show(kind); s.run(600);
      record(s, 'question: ' + t); shown++;
    }
    for (const t of [...reasons, ...zoneWhy]) {
      restore(s, page); put(s, t); s.x.sim_show(5); s.run(300);
      s.tap(...KEY_0, 200); s.tap(...KEY_SET, 600);
      record(s, 'keypad refusal: ' + t); shown++;
    }
  }
}

{
  const s = load(); s.boot(); s.run(1000);
  for (const [w, lines] of statusLines) for (const t of lines) {
    put(s, t);
    const n = s.x.sim_lines(w);
    if (n > 2) found.set('status ' + t, { line: 'Status strip | "' + t + '" | ' + n + ' lines at ' + w + ' px, the strip holds 2', where: 'status line' });
    shown++;
  }
}

const list = [...found.values()];
for (const f of list) console.log('DOES NOT FIT  ' + f.line + '\n              (' + f.where + ')');
console.log(list.length + ' problems on ' + states + ' screens, ' + keypads.size + ' keypads and ' + shown + ' messages and questions, ' +
  ((Date.now() - t0) / 1000).toFixed(0) + ' s');
process.exit(list.length ? 1 : 0);
