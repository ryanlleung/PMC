// The text port takes the remote command set only, in node:
//   node simulator/tests/remote-only.js
// Every other command (the old engineering commands, anything screen-only)
// is ERR 100. Homing is done on the screen, as on the cryostat.
const { load } = require('./sim-node.js');
let failed = 0;
function check(name, ok, detail) {
  console.log((ok ? 'ok   ' : 'FAIL ') + name + (ok || !detail ? '' : '\n     ' + detail));
  if (!ok) failed++;
}
const s = load();
s.boot(); s.set(5, 40000); s.run(2000);
check('boot sends nothing unasked', s.out === '', s.out);

const screenOnly = ['HELP', '?', 'VALVE STATE?', 'VALVE CAL', 'VALVE CLOSED', 'VALVE SETZERO', 'VALVE POSMAX 70000', 'CTRL?',
  'TUNE STEP', 'TUNE?', 'PID KP 3', 'CAL?', 'CAL VAC', 'CAL AIR', 'CAL PT 1 2', 'CFG?', 'CFG SAVE', 'NET?', 'NET IP DEFAULT',
  'MOT?', 'MOT MOVE 100', 'MOT OFF', 'LIM?', 'SENSE?', 'DATA?', 'BARO STATUS?', 'TREND?', 'TREND DUMP 3', 'VER?',
  'TOUCH?', 'PERF?', 'VALVE SAVED?', 'VALVE RESTORE'];
const bad = screenOnly.map(c => [c, s.cmd(c, 500)]).filter(([, r]) => r !== 'ERR 100 Unknown command\r\n')
  .map(([c, r]) => c + ' -> ' + JSON.stringify(r));
check('every other command: ERR 100', bad.length === 0, bad.join('\n     '));
check('nothing moved', s.get(5) === 40000);

// Home on the screen (Home valve, then Home in the question), then drive it remotely.
s.tap(269, 184); s.tap(346, 147); s.run(120000);   // Home valve (the Valve tile), Home
check('homed on the screen', s.cmd('MODE?;VALVE?') === 'MODE MANUAL;VALVE 0.00 %\r\n', s.cmd('MODE?;VALVE?'));
check('VALVE SET 20', s.cmd('VALVE SET 20', 60000) === '' && s.cmd('VALVE?;VALVE MOVING?') === 'VALVE 20.00 %;VALVE MOVING 0\r\n',
  s.cmd('VALVE?'));
s.cmd('SETPOINT 5'); s.cmd('MODE AUTO');
s.run(300000);
const poll = s.cmd('PRESSURE?;SETPOINT?;MODE?;VALVE?;ALARM?');
check('AUTO from the network', /^PRESSURE \d+\.\d{3} mbar;SETPOINT 5\.0 mbar;MODE AUTO;VALVE \d+\.\d\d %;ALARM NONE\r\n$/.test(poll), poll);
check('STOP ends AUTO', s.cmd('STOP;MODE?') === 'MODE MANUAL\r\n');
check('*IDN?', /^MercShore,iPC,[0-9A-F]{24},\d+\.\d+\.\d+\r\n$/.test(s.cmd('*IDN?')), s.cmd('*IDN?'));
console.log(failed ? failed + ' failed' : 'all passed');
process.exit(failed ? 1 : 0);
