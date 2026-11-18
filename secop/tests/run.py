"""Run the Frappy driver against a stand-in board.

    python secop/tests/run.py          the fake board (tests/fake_ipc.py)
    python secop/tests/run.py --sim    the simulated board: the firmware
                                       itself, compiled to WebAssembly
                                       (simulator/tests/tcp-board.js, needs node)

Needs frappy-core (pip install frappy-core). Starts the board and
frappy-server on spare local ports, talks SECoP to it with the Frappy
client, and prints PASS or FAIL for each check.

The fake board covers every state through its TEST lines. The simulated
board is started three times: Network control On, Network control Off
(read only) and shut down (OFF), as only the screen reaches those.
"""

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from frappy.client import SecopClient

HERE = Path(__file__).resolve().parent
SECOP = HERE.parent
REPO = SECOP.parent
failures = 0

# Valve positions and setpoints that suit each board's pressure: the fake
# gives 0.2 mbar per % open, the simulated flow line about 3.8 mbar at 40 %
# and 5.4 mbar at 50 % (the simulated board runs ten times faster than the
# clock).
PROFILES = {
    'fake': dict(valve1=20.0, sp1=3.0, valve2=10.0, sp2=2.0, settle=60),
    'sim': dict(valve1=50.0, sp1=6.0, valve2=40.0, sp2=4.0, settle=90),
}
SIM_SPEED = 10


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


def refused(action, text):
    """True if action raises an error whose message holds text."""
    try:
        action()
    except Exception as e:
        return text in str(e)
    return False


def fake_test(port, line):
    """A TEST line to the fake board (its own connection; it takes several)."""
    with socket.create_connection(('127.0.0.1', port)) as s:
        s.sendall((line + '\n').encode())
        time.sleep(0.3)


def start_board(kind, port, *flags):
    if kind == 'fake':
        cmd = [sys.executable, str(HERE / 'fake_ipc.py'), str(port)]
    else:
        cmd = ['node', str(REPO / 'simulator' / 'tests' / 'tcp-board.js'), str(port), str(SIM_SPEED), *flags]
    board = subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True)
    board.stdout.readline()   # the port is open
    return board


def with_server(kind, board_port, secop_port, checks, *flags):
    """Start the board and the Frappy server, run checks(client, get), stop both."""
    tmp = tempfile.mkdtemp()
    env = dict(os.environ, IPC_URI=f'tcp://127.0.0.1:{board_port}',
               FRAPPY_CONFDIR=str(SECOP / 'cfg'), FRAPPY_LOGDIR=tmp, FRAPPY_PIDDIR=tmp,
               PYTHONPATH=str(SECOP) + os.pathsep + os.environ.get('PYTHONPATH', ''))
    board = start_board(kind, board_port, *flags)
    server = subprocess.Popen(['frappy-server', '-q', '-p', str(secop_port),
                               '-c', str(SECOP / 'cfg' / 'ipc_cfg.py'), 'ipc'], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        client = SecopClient(f'localhost:{secop_port}')
        wait_until(lambda: client.connect(1) or True, 20)
        get = lambda m, p: client.getParameter(m, p)[0]
        checks(client, get)
        client.disconnect()
    finally:
        server.terminate()
        err, _ = server.communicate(timeout=10)
        board.terminate()
        board.wait(timeout=10)
    return err


def main_checks(kind):
    pr = PROFILES[kind]

    def checks(client, get):
        check('reads a pressure', wait_until(lambda: get('pressure', 'value') > 0))
        check('reads the valve %', wait_until(lambda: abs(get('valve', 'value') - 5.0) < 0.5),
              get('valve', 'value'))
        check('starts in MANUAL', get('pressure', 'control_active') is False and
              get('valve', 'controlled_by').name == 'self')

        client.setParameter('valve', 'target', pr['valve1'])
        check(f'valve move reaches {pr["valve1"]:g} %',
              wait_until(lambda: abs(get('valve', 'value') - pr['valve1']) < 0.2 and
                         get('valve', 'status')[0].name == 'IDLE', 30),
              (get('valve', 'value'), get('valve', 'status')))

        client.setParameter('pressure', 'target', pr['sp1'])
        check(f'setpoint {pr["sp1"]:g} mbar read back',
              wait_until(lambda: abs(get('pressure', 'target') - pr['sp1']) < 1e-6), get('pressure', 'target'))
        check('writing the setpoint starts AUTO',
              wait_until(lambda: get('pressure', 'control_active') is True and
                         get('valve', 'controlled_by').name == 'pressure'))
        check(f'AUTO settles near {pr["sp1"]:g} mbar',
              wait_until(lambda: abs(get('pressure', 'value') - pr['sp1']) < 0.5 and
                         get('pressure', 'status')[0].name == 'IDLE', pr['settle']),
              (get('pressure', 'value'), get('pressure', 'status')))

        client.setParameter('valve', 'target', pr['valve2'])
        check('writing the valve ends AUTO',
              wait_until(lambda: get('pressure', 'control_active') is False and
                         get('valve', 'controlled_by').name == 'self'))
        check(f'valve then reaches {pr["valve2"]:g} %',
              wait_until(lambda: abs(get('valve', 'value') - pr['valve2']) < 0.2, 30), get('valve', 'value'))

        client.setParameter('pressure', 'target', pr['sp2'])
        check('AUTO again', wait_until(lambda: get('pressure', 'control_active') is True))
        client.execCommand('pressure', 'stop')
        check('stop ends AUTO', wait_until(lambda: get('pressure', 'control_active') is False))

        client.execCommand('pressure', 'reset')
        check('reset accepted', True)
        client.execCommand('pressure', 'ok_warning')
        check('ok_warning accepted', True)
        check('setpoint above 1000 mbar refused (the driver\'s range)',
              refused(lambda: client.setParameter('pressure', 'target', 2000), ''))

        # a board without the barometer replies BAROMETER NONE: status says so instead
        baro = get('room_pressure', 'value')
        check('room pressure read', 900 < baro < 1100
              or get('room_pressure', 'status')[1].startswith('no reading'), baro)

        if kind == 'fake':
            fake_checks(client, get)

    return checks


def fake_checks(client, get):
    """The states only the screen reaches, through the fake board's TEST lines."""
    port = PORTS['fake'][0]

    fake_test(port, 'TEST FAULT F-1')
    check('fault: pressure status ERROR', wait_until(
        lambda: get('pressure', 'status')[0].name == 'ERROR' and get('pressure', 'alarm') == 'F-1'))
    check('fault: setpoint refused (ERR 203)',
          refused(lambda: client.setParameter('pressure', 'target', 2.0), 'Reset the fault first'))
    client.execCommand('pressure', 'reset')
    check('reset clears the fault', wait_until(lambda: get('pressure', 'alarm') == ''))

    fake_test(port, 'TEST REMOTE 0')
    check('Network control Off: setpoint refused (ERR 211)',
          refused(lambda: client.setParameter('pressure', 'target', 2.0), 'Read only over the network'))
    check('Network control Off: stop refused (ERR 211)',
          refused(lambda: client.execCommand('pressure', 'stop'), 'Read only over the network'))
    p0 = get('pressure', 'value')
    check('Network control Off: readings still update',
          wait_until(lambda: client.getParameter('pressure', 'value', trycache=False)[0] != p0, 10))
    fake_test(port, 'TEST REMOTE 1')

    client.setParameter('valve', 'target', 10.0)
    wait_until(lambda: abs(get('valve', 'value') - 10.0) < 0.2, 20)
    fake_test(port, 'TEST SHUTDOWN')
    check('shutting down: both modules BUSY', wait_until(
        lambda: get('pressure', 'status')[0].name == 'BUSY' and get('valve', 'status')[0].name == 'BUSY'
        and get('pressure', 'status')[1].startswith('shutting down')))
    check('then OFF: both modules DISABLED', wait_until(
        lambda: get('pressure', 'status')[0].name == 'DISABLED' and get('valve', 'status')[0].name == 'DISABLED', 20),
        (get('pressure', 'status'), get('valve', 'status')))
    off_checks(client, get)
    fake_test(port, 'TEST ON')
    check('turned on at the screen: IDLE again', wait_until(
        lambda: get('pressure', 'status')[0].name == 'IDLE' and get('valve', 'status')[0].name == 'IDLE'))


def off_checks(client, get):
    check('OFF: setpoint refused (ERR 213)',
          refused(lambda: client.setParameter('pressure', 'target', 2.0), 'Controller is off'))
    check('OFF: valve move refused (ERR 213)',
          refused(lambda: client.setParameter('valve', 'target', 5.0), 'Controller is off'))
    check('OFF: still not in AUTO', get('pressure', 'control_active') is False)


def read_only_checks(client, get):
    check('reads a pressure', wait_until(lambda: get('pressure', 'value') > 0))
    check('Network control Off: setpoint refused (ERR 211)',
          refused(lambda: client.setParameter('pressure', 'target', 20.0), 'Read only over the network'))
    check('Network control Off: valve move refused (ERR 211)',
          refused(lambda: client.setParameter('valve', 'target', 8.0), 'Read only over the network'))
    check('Network control Off: stop refused (ERR 211)',
          refused(lambda: client.execCommand('valve', 'stop'), 'Read only over the network'))
    check('Network control Off: still MANUAL at 5 %',
          get('pressure', 'control_active') is False and abs(get('valve', 'value') - 5.0) < 0.2)


def sim_off_checks(client, get):
    check('OFF: both modules DISABLED', wait_until(
        lambda: get('pressure', 'status')[0].name == 'DISABLED' and get('valve', 'status')[0].name == 'DISABLED'),
        (get('pressure', 'status'), get('valve', 'status')))
    off_checks(client, get)


def log_errors(err, what):
    bad = [l for l in err.splitlines() if 'ERROR' in l or 'Traceback' in l]
    check(f'server log has no errors ({what})', not bad, '; '.join(bad[:3]))


PORTS = {'fake': (15601, 15701), 'sim': (15602, 15702)}

if __name__ == '__main__':
    kind = 'sim' if '--sim' in sys.argv else 'fake'
    board_port, secop_port = PORTS[kind]
    print(f'-- {kind} board')
    log_errors(with_server(kind, board_port, secop_port, main_checks(kind)), kind)
    if kind == 'sim':
        print('-- simulated board, Network control Off')
        log_errors(with_server(kind, board_port + 10, secop_port + 10, read_only_checks, '--remote-off'),
                   'read only')
        print('-- simulated board, shut down (OFF)')
        log_errors(with_server(kind, board_port + 20, secop_port + 20, sim_off_checks, '--off'), 'OFF')
    print('all passed' if not failures else f'{failures} failed')
    sys.exit(1 if failures else 0)
