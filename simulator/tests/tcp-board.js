// The simulated board on a TCP port, in real time, for a driver to talk to:
//   node simulator/tests/tcp-board.js [port] [speed] [--remote-off]
// Port 8888 by default, as the board; speed runs the simulation faster than
// the clock (2 = twice as fast). The valve is homed and set to 5 % before
// the port opens. Remote control is On, as after an operator turned it On
// on the screen; --remote-off turns it Off for read-only tests (set commands
// answer ERR 211). One client at a time; a new one replaces the old, as on
// the board.
const net = require('net');
const { load } = require('./sim-node.js');
const argv = process.argv.slice(2).filter(a => a !== '--remote-off');
const port = Number(argv[0] || 8888), speed = Number(argv[1] || 1);
const remoteOff = process.argv.includes('--remote-off');

const s = load(); s.boot(undefined, { remote: true }); s.set(5, 40000); s.run(2000);
// Homing is screen-only in the firmware: use the same test hook as run.js.
if (!s.home()) throw new Error('Cannot home the simulated board: ' + s.why());
const move = s.cmd('VALV SET 5', 30000);
const ready = s.cmd('MODE?;VALV?;VALV MOVING?;ALRM?');
if (move || ready !== 'MODE MANUAL;VALV 5.00 %;VALV MOVING 0;ALRM NONE\r\n')
  throw new Error('Simulated board not ready: ' + move + ready);
if (remoteOff) s.x.sim_remote(0);
s.out = '';

let client = null, rx = '';
setInterval(() => {
  s.run(10 * speed);
  if (s.out && client) client.write(s.out);
  s.out = '';
}, 10);

const server = net.createServer(c => {
  if (client) client.destroy();
  client = c; rx = '';
  c.on('data', d => {
    rx += d.toString('latin1');
    let i;
    while ((i = rx.search(/[\r\n]/)) >= 0) {
      const line = rx.slice(0, i);
      rx = rx.slice(i + 1);
      if (line) s.send(line);
    }
  });
  c.on('close', () => { if (client === c) client = null; });
  c.on('error', () => {});
}).listen(port, '127.0.0.1', () => console.log('simulated mm4 on port ' + server.address().port + ', ' + ready.trim()));
