#!/usr/bin/env python3
"""Tests for pmc_gains.py.

    python test_pmc_gains.py          # fit, table and run against a fake board (seconds)
    python test_pmc_gains.py --sim    # also tune and run on the simulated board (node, about 1 min)
"""
import io
import math
import os
import random
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pmc_gains as g   # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))


class FakeBoard:
    """Text port with PRES SP?, MODE?, PID? and PID SET only."""
    def __init__(self, sp=5.0, kp=4.0, ti=0.25, pid=True):
        self.sp, self.kp, self.ti, self.pid = sp, kp, ti, pid
        self.sets = []
        self.srv = socket.socket()
        self.srv.bind(('127.0.0.1', 0))
        self.srv.listen(1)
        self.port = self.srv.getsockname()[1]
        threading.Thread(target=self.serve, daemon=True).start()

    def serve(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            f = c.makefile('rwb')
            for line in f:
                out = []
                for part in line.decode().strip().split(';'):
                    if part == 'PRES SP?':
                        out.append(f'PRES SP {self.sp:.1f} mbar')
                    elif part == 'MODE?':
                        out.append('MODE AUTO')
                    elif part == 'PID?' and self.pid:
                        out.append(f'PID {self.kp:.2f},{self.ti:.2f},0.00')
                    elif part.startswith('PID SET ') and self.pid:
                        self.kp, self.ti = [float(v) for v in part[8:].split(',')]
                        self.sets.append((self.kp, self.ti))
                    else:
                        out = ['ERR 100 Unknown command']
                        break
                f.write((';'.join(out) + '\r\n').encode())
                f.flush()

    def close(self):
        self.srv.close()


def table_file(rows):
    fd, path = tempfile.mkstemp(suffix='.csv')
    with os.fdopen(fd, 'w') as f:
        f.write('sp_mbar,kp,ti_min\n')
        for r in rows:
            f.write(','.join(str(v) for v in r) + '\n')
    return path


class Tests(unittest.TestCase):
    def test_fit_recovers_a_step(self):
        rnd = random.Random(1)
        K, delay, tau, dx = 0.8, 7.0, 45.0, 0.5
        t = [float(i) for i in range(1, 300)]
        dp = [(K * dx * (1 - math.exp(-(ti - delay) / tau)) if ti > delay else 0.0) + rnd.gauss(0, 0.01) for ti in t]
        f = g.fit_step(t, dp, dx)
        self.assertAlmostEqual(f['K'], K, delta=0.03)
        self.assertAlmostEqual(f['delay'], delay, delta=2.0)
        self.assertAlmostEqual(f['tau'], tau, delta=6.0)
        # Downward step: same K.
        f = g.fit_step(t, [-v for v in dp], -dx)
        self.assertAlmostEqual(f['K'], K, delta=0.03)

    def test_simc(self):
        kp, ti = g.simc(0.8, 7.0, 45.0)
        self.assertAlmostEqual(kp, 45.0 / (0.8 * 14.0), places=6)
        self.assertAlmostEqual(ti, 45.0 / 60, places=6)     # tau < 8 delay
        kp, ti = g.simc(0.8, 2.0, 300.0)
        self.assertAlmostEqual(ti, 16.0 / 60, places=6)     # 8 delay < tau

    def test_table_blend(self):
        tab = g.load_table(table_file([(10, 1.0, 0.5), (2, 0.2, 0.2), (5, 0.5, 0.3)]))
        self.assertEqual([r[0] for r in tab], [2, 5, 10])
        self.assertEqual(g.gains_for(tab, 2), (0.2, 0.2))
        self.assertEqual(g.gains_for(tab, 1), (0.2, 0.2))      # below the table: first row
        self.assertEqual(g.gains_for(tab, 25), (1.0, 0.5))     # above: last row
        kp, ti = g.gains_for(tab, math.sqrt(50))               # log midpoint of 5 and 10
        self.assertAlmostEqual(kp, round(math.sqrt(0.5), 2))
        self.assertAlmostEqual(ti, round(math.sqrt(0.15), 2))

    def test_run_sets_the_blended_gains(self):
        b = FakeBoard(sp=5.0, kp=4.0, ti=0.25)
        path = table_file([(2, 0.2, 0.2), (5, 0.5, 0.3), (10, 1.0, 0.5)])
        try:
            with redirect_stdout(io.StringIO()) as out:
                g.main(['run', f'127.0.0.1:{b.port}', path, '--once'])
            self.assertEqual(b.sets, [(0.5, 0.3)])
            self.assertIn('KP 4.00 -> 0.50', out.getvalue())
            # Same setpoint, gains already right: nothing written.
            with redirect_stdout(io.StringIO()):
                g.main(['run', f'127.0.0.1:{b.port}', path, '--once'])
            self.assertEqual(b.sets, [(0.5, 0.3)])
            # Dry run writes nothing.
            b.sp = 10.0
            with redirect_stdout(io.StringIO()) as out:
                g.main(['run', f'127.0.0.1:{b.port}', path, '--once', '--dry-run'])
            self.assertEqual(b.sets, [(0.5, 0.3)])
            self.assertIn('would set KP 1.00 TI 0.50', out.getvalue())
        finally:
            b.close()

    def test_run_without_pid_commands_stops(self):
        b = FakeBoard(pid=False)
        try:
            with self.assertRaises(SystemExit) as e:
                g.main(['run', f'127.0.0.1:{b.port}', table_file([(5, 0.5, 0.3)]), '--once'])
            self.assertIn('0.15.8', str(e.exception.code))
        finally:
            b.close()


def sim_tune():
    """One setpoint on the simulated board (simulator/tests/tcp-board.js) at 20x speed."""
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    port = s.getsockname()[1]
    s.close()
    board = subprocess.Popen(['node', os.path.join(ROOT, 'simulator', 'tests', 'tcp-board.js'), str(port), '20'],
                             stdout=subprocess.PIPE, text=True)
    try:
        print(board.stdout.readline().strip())
        out = os.path.join(tempfile.mkdtemp(), 'sim.csv')
        g.main(['tune', f'127.0.0.1:{port}', '--sp', '3', '--step', '1', '--out', out, '--speed', '20'])
        tab = g.load_table(out)
        assert len(tab) == 1 and tab[0][1] > 0 and tab[0][2] > 0, tab
        print('sim tune: OK')
        # run (firmware 0.15.8 on): the board takes the table's gains for its setpoint.
        g.main(['run', f'127.0.0.1:{port}', out, '--once'])
        pid = g.Link(f'127.0.0.1:{port}').ask('PID?')[0]
        assert pid == 'PID %.2f,%.2f,0.00' % g.gains_for(tab, 3.0), pid
        print('sim run: OK')
    finally:
        board.terminate()


if __name__ == '__main__':
    sim = '--sim' in sys.argv
    if sim:
        sys.argv.remove('--sim')
    r = unittest.main(exit=False).result
    if not r.wasSuccessful():
        sys.exit(1)
    if sim:
        sim_tune()
