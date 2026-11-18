// The simulated board on a TCP port, in real time, for a driver to talk to:
//   node simulator/tests/tcp-board.js [port] [speed] [--remote-off] [--off]
// Port 8888 by default, as the board; speed runs the simulation faster than
// the clock (2 = twice as fast). The valve is homed and set to 5 % before
// the port opens, and every PID zone has KP 4.00, TI 0.25: the factory
// table is a cryostat's, and the simulated flow line gives far less
// pressure per % open (0.8 to 12.5 mbar over 20 to 100 %), so AUTO on it
// with the factory gains takes many minutes. Network control is On, as
// after an operator turned it On on the screen; --remote-off turns it Off
// for read-only tests (set commands answer ERR 211). One client at a time;
// a new one replaces the old, as on the board.
// --off shuts the board down after that (the valve closes to 0 %, then
// OFF), as after Shut down on the screen: MODE AUTO, MODE MANUAL and
// VALVE SET answer ERR 213.
const net = require('net');
const { load } = require('./sim-node.js');
const argv = process.argv.slice(2).filter(a => !a.startsWith('--'));
const port = Number(argv[0] || 8888), speed = Number(argv[1] || 1);
const remoteOff = process.argv.includes('--remote-off');

const s = load(); s.boot(undefined, { remote: true }); s.set(5, 40000); s.run(2000);
// Homing is screen-only in the firmware: use the same test hook as run.js.
if (!s.home()) throw new Error('Cannot home the simulated board: ' + s.why());
const move = s.cmd('VALVE SET 5', 30000) + s.cmd('PIDS SET 1,4,0.25,0;PIDS SET 2,4,0.25,0;PIDS SET 3,4,0.25,0;PIDS SET 4,4,0.25,0');
const ready = s.cmd('MODE?;VALVE?;VALVE MOVING?;ALARM?');
if (move || ready !== 'MODE MANUAL;VALVE 5.00 %;VALVE MOVING 0;ALARM NONE\r\n')
  throw new Error('Simulated board not ready: ' + move + ready);
if (remoteOff) s.x.sim_remote(0);
if (process.argv.includes('--off')) {
  if (!s.shutdown(60000) || s.cmd('MODE?') !== 'MODE OFF\r\n') throw new Error('Cannot shut down the simulated board');
}
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
}).listen(port, '127.0.0.1', () => console.log('simulated iPC on port ' + server.address().port + ', ' + ready.trim() + (process.argv.includes('--off') ? ', then shut down: OFF' : '')));
