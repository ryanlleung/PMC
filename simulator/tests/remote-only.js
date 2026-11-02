// The operator build's command set (PMC_ENG_COMMANDS 0), in node:
//   PMC_ENG_COMMANDS=0 sh simulator/wasm/build.sh && node simulator/tests/remote-only.js
// Only the remote set answers; every engineering or screen-only command is
// ERR 100. Homing is done on the screen, as on the cryostat.
const path = require('path');
const { load } = require('./sim-node.js');
let failed = 0;
function check(name, ok, detail) {
  console.log((ok ? 'ok   ' : 'FAIL ') + name + (ok || !detail ? '' : '\n     ' + detail));
  if (!ok) failed++;
}
const s = load(path.join(__dirname, '..', 'wasm', 'out', 'pmc-op.wasm'));
s.boot(); s.set(5, 40000); s.run(2000);
check('boot sends nothing unasked', s.out === '', s.out);

const screenOnly = ['HELP', '?', 'VALVE?', 'VALVE CAL', 'VALVE CLOSED', 'VALVE SETZERO', 'VALVE POSMAX 70000', 'CTRL?',
  'TUNE STEP', 'TUNE?', 'PID?', 'PID KP 3', 'CAL?', 'CAL VAC', 'CAL PT 1 2', 'CFG?', 'CFG SAVE', 'NET?', 'NET IP DEFAULT',
  'MOT?', 'MOT MOVE 100', 'MOT OFF', 'LIM?', 'SENSE?', 'DATA?', 'BARO STATUS?', 'TREND?', 'TREND DUMP 3', 'VER?',
  'TOUCH?', 'PERF?', 'VALVE SAVED?', 'VALVE RESTORE'];
const bad = screenOnly.map(c => [c, s.cmd(c, 500)]).filter(([, r]) => r !== 'ERR 100 Unknown command\r\n')
  .map(([c, r]) => c + ' -> ' + JSON.stringify(r));
check('engineering and screen-only commands: ERR 100', bad.length === 0, bad.join('\n     '));
check('nothing moved', s.get(5) === 40000);

// Home on the screen (Home valve, then Home in the question), then drive it remotely.
s.tap(415, 72); s.tap(346, 147); s.run(120000);
check('homed on the screen', s.cmd('MODE?;VALV?') === 'MODE MANUAL;VALV 0.00 %\r\n', s.cmd('MODE?;VALV?'));
check('VALV SET 20', s.cmd('VALV SET 20', 60000) === '' && s.cmd('VALV?;VALV MOVING?') === 'VALV 20.00 %;VALV MOVING 0\r\n',
  s.cmd('VALV?'));
s.cmd('PRES SP 5'); s.cmd('MODE AUTO');
s.run(300000);
const poll = s.cmd('PRES?;PRES SP?;MODE?;VALV?;ALRM?');
check('AUTO from the network', /^PRES \d+\.\d{3} mbar;PRES SP 5\.0 mbar;MODE AUTO;VALV \d+\.\d\d %;ALRM NONE\r\n$/.test(poll), poll);
check('STOP ends AUTO', s.cmd('STOP;MODE?') === 'MODE MANUAL\r\n');
check('*IDN?', /^MercShore,iPC,[0-9A-F]{24},\d+\.\d+\.\d+\r\n$/.test(s.cmd('*IDN?')), s.cmd('*IDN?'));
console.log(failed ? failed + ' failed' : 'all passed');
process.exit(failed ? 1 : 0);
