"""Run the Frappy driver against the fake mm4.

    python secop/tests/run.py

Needs frappy-core (pip install frappy-core). Starts fake_mm4.py and
frappy-server on spare local ports, talks SECoP to it with the Frappy
client, and prints PASS or FAIL for each check.
"""

import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from frappy.client import SecopClient

HERE = Path(__file__).resolve().parent
SECOP = HERE.parent
failures = 0


def check(name, ok, detail=''):
    global failures
    print(('PASS ' if ok else 'FAIL ') + name + (f'  ({detail})' if detail and not ok else ''))
    failures += not ok


def wait_until(pred, timeout=15):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pred():
                return True
        except Exception:
            pass
        time.sleep(0.3)
    return False


def run(board_port, secop_port):
    tmp = tempfile.mkdtemp()
    env = dict(os.environ, MM4_URI=f'tcp://127.0.0.1:{board_port}',
               FRAPPY_CONFDIR=str(SECOP / 'cfg'), FRAPPY_LOGDIR=tmp, FRAPPY_PIDDIR=tmp,
               PYTHONPATH=str(SECOP) + os.pathsep + os.environ.get('PYTHONPATH', ''))
    board = subprocess.Popen([sys.executable, str(HERE / 'fake_mm4.py'), str(board_port)], env=env,
                             stdout=subprocess.DEVNULL)
    time.sleep(0.5)
    server = subprocess.Popen(['frappy-server', '-q', '-p', str(secop_port),
                               '-c', str(SECOP / 'cfg' / 'mm4_cfg.py'), 'mm4'], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        client = SecopClient(f'localhost:{secop_port}')
        wait_until(lambda: client.connect(1) or True, 20)
        get = lambda m, p: client.getParameter(m, p)[0]

        check('reads a pressure', wait_until(lambda: get('pressure', 'value') > 0))
        check('reads the valve %', wait_until(lambda: abs(get('valve', 'value') - 5.0) < 0.5),
              get('valve', 'value'))
        check('starts in MANUAL', get('pressure', 'control_active') is False and
              get('valve', 'controlled_by').name == 'self')

        client.setParameter('valve', 'target', 20.0)
        check('valve move reaches 20 %',
              wait_until(lambda: abs(get('valve', 'value') - 20.0) < 0.2 and
                         get('valve', 'status')[0].name == 'IDLE', 30),
              (get('valve', 'value'), get('valve', 'status')))

        client.setParameter('pressure', 'target', 3.0)
        check('setpoint 3.0 mbar read back',
              wait_until(lambda: abs(get('pressure', 'target') - 3.0) < 1e-6), get('pressure', 'target'))
        check('writing the setpoint starts AUTO',
              wait_until(lambda: get('pressure', 'control_active') is True and
                         get('valve', 'controlled_by').name == 'pressure'))
        check('AUTO settles near 3.0 mbar',
              wait_until(lambda: abs(get('pressure', 'value') - 3.0) < 0.5 and
                         get('pressure', 'status')[0].name == 'IDLE', 60),
              (get('pressure', 'value'), get('pressure', 'status')))

        client.setParameter('valve', 'target', 10.0)
        check('writing the valve ends AUTO',
              wait_until(lambda: get('pressure', 'control_active') is False and
                         get('valve', 'controlled_by').name == 'self'))
        check('valve then reaches 10 %',
              wait_until(lambda: abs(get('valve', 'value') - 10.0) < 0.2, 30), get('valve', 'value'))

        client.setParameter('pressure', 'target', 2.0)
        check('AUTO again', wait_until(lambda: get('pressure', 'control_active') is True))
        client.execCommand('pressure', 'stop')
        check('stop ends AUTO', wait_until(lambda: get('pressure', 'control_active') is False))

        client.execCommand('pressure', 'reset')
        check('reset accepted', True)
        try:
            client.setParameter('pressure', 'target', 2000)
            check('setpoint out of range refused', False, 'no error raised')
        except Exception:
            check('setpoint out of range refused', True)

        # a board without the barometer replies BAROMETER NONE: status says so instead
        baro = get('room_pressure', 'value')
        check('room pressure read', abs(baro - 1013.25) < 0.01
              or get('room_pressure', 'status')[1].startswith('no reading'), baro)
        client.disconnect()
    finally:
        server.terminate()
        err, _ = server.communicate(timeout=10)
        board.terminate()
        board.wait(timeout=10)
        bad = [l for l in err.splitlines() if 'ERROR' in l or 'Traceback' in l]
        check('server log has no errors', not bad, '; '.join(bad[:3]))


if __name__ == '__main__':
    run(15601, 15701)
    print('all passed' if not failures else f'{failures} failed')
    sys.exit(1 if failures else 0)
