// Exercise the actual TCP fixture startup and its remote-control switch.
//   node simulator/tests/tcp-board-test.js
const assert = require('node:assert/strict');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { once } = require('node:events');

async function fixture(remoteOff) {
  const child = spawn(process.execPath, [path.join(__dirname, 'tcp-board.js'), '0',
    ...(remoteOff ? ['--remote-off'] : [])], { stdio: ['ignore', 'pipe', 'pipe'] });
  let output = '', errors = '', socket;
  const timeout = setTimeout(() => child.kill(), 15000);
  child.stderr.on('data', data => { errors += data; });
  try {
    const port = await new Promise((resolve, reject) => {
      child.on('error', reject);
      child.once('exit', code => reject(new Error('Fixture exited: ' + code + '\n' + errors)));
      child.stdout.on('data', data => {
        output += data;
        const ready = output.match(/simulated mm4 on port (\d+), MODE MANUAL;VALVE 5\.00 %/);
        if (ready) resolve(Number(ready[1]));
      });
    });
    socket = net.createConnection({ host: '127.0.0.1', port });
    await once(socket, 'connect');
    let received = '', waiting;
    socket.on('data', data => {
      received += data.toString();
      const end = received.indexOf('\r\n');
      if (waiting && end >= 0) {
        const { resolve } = waiting; waiting = null;
        const line = received.slice(0, end); received = received.slice(end + 2);
        resolve(line);
      }
    });
    socket.on('error', error => { if (waiting) waiting.reject(error); });
    socket.on('close', () => { if (waiting) waiting.reject(new Error('Fixture connection closed')); });
    const query = line => new Promise((resolve, reject) => {
      waiting = { resolve, reject };
      socket.write(line + '\r\n');
    });
    assert.equal(await query('MODE?;VALVE?;VALVE MOVING?;ALARM?'),
      'MODE MANUAL;VALVE 5.00 %;VALVE MOVING 0;ALARM NONE');
    assert.equal(await query('SETPOINT 5;SETPOINT?'), remoteOff
      ? 'ERR 211 Read only over the network' : 'SETPOINT 5.0 mbar');
    assert.equal(await query('STOP;MODE?'), remoteOff
      ? 'ERR 211 Read only over the network' : 'MODE MANUAL');
    assert.equal(errors, '');
    console.log('ok   TCP fixture homed at 5 %, remote control ' + (remoteOff ? 'Off' : 'On'));
  } finally {
    if (socket) socket.destroy();
    if (child.exitCode === null && child.signalCode === null) {
      child.kill();
      await once(child, 'exit');
    }
    clearTimeout(timeout);
  }
}

(async () => {
  await fixture(false);
  await fixture(true);
  console.log('all passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
