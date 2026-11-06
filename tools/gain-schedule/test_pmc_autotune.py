#!/usr/bin/env python3
"""Tests for pmc_autotune.py, on a valve and cryostat model (no board, seconds).

    python test_pmc_autotune.py
"""
import math
import os
import random
import sys
import tempfile
import unittest
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pmc_autotune as at   # noqa: E402
import pmc_gains as g       # noqa: E402

SLEW = 0.61   # %/s, the drive


class Plant:
    """Pressure from the valve: a static curve, a dead time and a lag, with
    noise, the drive's travel rate and (optionally) a stiction band in the
    opening direction, stepped once per 0.1 s."""

    def __init__(self, K=2.0, L=2.0, tau=10.0, floor=0.67, curve=None, noise=0.01, stick=0.0, slew=SLEW, seed=1):
        self.K, self.L, self.tau, self.floor, self.curve = K, L, tau, floor, curve
        self.noise, self.stick, self.slew = noise, stick, slew
        self.rnd = random.Random(seed)
        self.v = self.cmd = 5.0     # valve position and where it is going, %
        self.eff = self.v            # the opening the gas sees (stiction)
        self.t = 0.0
        self.hist = []
        self.x = self.static(self.v)

    def static(self, v):
        return self.curve(v) if self.curve else self.floor + self.K * max(v, 0.0)

    def run_to(self, t):
        dt = 0.1
        while self.t < t - 1e-9:
            dv = max(-self.slew * dt, min(self.slew * dt, self.cmd - self.v)) if self.slew else self.cmd - self.v
            self.v += dv
            if self.v < self.eff or self.v > self.eff + self.stick:
                self.eff = self.v if self.v < self.eff else self.v - self.stick
            self.hist.append(self.eff)
            n = int(round(self.L / dt))
            vin = self.hist[-1 - n] if len(self.hist) > n else self.hist[0]
            target = self.static(vin)
            self.x += (target - self.x) * dt / self.tau
            self.t += dt

    def p(self):
        return self.x + self.rnd.gauss(0, self.noise)


def run_relay(plant, sp, x0, d, eps):
    """The relay logic against the model, one sample a second."""
    R = at.Relay(sp, x0, d, eps)
    plant.cmd = x0
    plant.run_to(plant.t + 120)
    t0 = plant.t
    n = 0
    while not R.done and n < 2000:
        n += 1
        plant.run_to(t0 + n)
        cmd = R.feed(plant.t - t0, plant.p(), round(plant.v, 2))
        if cmd is not None:
            plant.cmd = cmd
    return R


def ultimate_gain(K, L, tau, dt=1.0):
    """Ku of the same sampled loop with a P controller and one sample of
    decision delay, by bisection on whether a kick grows or dies (no slew,
    no noise)."""
    def grows(k):
        p = Plant(K=K, L=L, tau=tau, noise=0.0, slew=0.0, floor=0.0)
        p.static = lambda v: K * v           # no ends, so a growing swing is not clipped
        p.v = p.cmd = p.eff = 10.0
        p.hist, y0 = [], p.static(10.0)
        p.x = y0 + 0.05
        ys = []
        for i in range(1, 600):
            p.run_to(i * dt)
            ys.append(p.x - y0)
            p.cmd = 10.0 - k * ys[-1]
        return max(abs(y) for y in ys[-150:]) > max(abs(y) for y in ys[150:300])
    lo, hi = 0.01, 50.0
    for _ in range(40):
        mid = math.sqrt(lo * hi)
        lo, hi = (lo, mid) if grows(mid) else (mid, hi)
    return math.sqrt(lo * hi)


class Relay(unittest.TestCase):
    def test_ku_and_pu_on_a_linear_plant(self):
        # Without the drive's travel rate the relay result should match the
        # sampled loop's own ultimate gain (one sample of decision delay).
        for K, L, tau in [(2.7, 2.0, 20.0), (1.8, 1.5, 7.5), (8.0, 2.0, 5.0)]:
            plant = Plant(K=K, L=L, tau=tau, noise=0.002, slew=0.0)
            R = run_relay(plant, plant.static(5.0), 5.0, 0.4, 0.02)
            self.assertTrue(R.done and not R.why, R.why)
            r = R.result()
            ku = ultimate_gain(K, L, tau)
            self.assertAlmostEqual(r['Ku'] / ku, 1.0, delta=0.3, msg='K %g: relay %.2f, loop %.2f' % (K, r['Ku'], ku))
            self.assertTrue(r['steady'])

    def test_grows_the_step_when_nothing_crosses(self):
        # A stiction band wider than 2d: no crossing until d is doubled.
        plant = Plant(K=2.0, L=2.0, tau=10.0, stick=1.0, noise=0.005)
        R = run_relay(plant, plant.static(5.0), 5.0, 0.2, 0.05)
        self.assertTrue(R.done and not R.why, R.why)
        self.assertGreater(R.d, 0.5)

    def test_shrinks_the_step_when_the_swing_is_big(self):
        plant = Plant(K=8.0, L=2.0, tau=5.0, noise=0.005)
        sp = plant.static(2.0)
        R = run_relay(plant, sp, 2.0, 3.0, 0.05)
        self.assertTrue(R.done and not R.why, R.why)
        self.assertLess(R.d, 3.0)
        self.assertLess(R.result()['a_mbar'], at.AMP_HI * sp * 1.2)

    def test_fails_when_the_pressure_runs_away(self):
        plant = Plant(K=2.0, L=2.0, tau=10.0)
        R = at.Relay(5.0, 5.0, 0.4, 0.05)
        R.feed(0, 5.0, 5.0)
        R.feed(1, 11.0, 5.0)
        self.assertTrue(R.done)
        self.assertIn('too far', R.why)
        del plant


class Rules(unittest.TestCase):
    def test_gains(self):
        self.assertEqual(at.relay_gains(4.0, 9.0), (1.0, 0.3))
        self.assertEqual(at.relay_gains(0.001, 0.1), (g.KP_MIN, g.TI_MIN))

    def test_trim(self):
        f, _ = at.trim_rule({'overshoot_pct': 25, 'settle_s': 40, 'reversals': 1, 'saturated': False}, 10)
        self.assertEqual(f, at.TRIM_DOWN)
        f, _ = at.trim_rule({'overshoot_pct': 2, 'settle_s': 40, 'reversals': 4, 'saturated': False}, 10)
        self.assertEqual(f, at.TRIM_DOWN)
        f, _ = at.trim_rule({'overshoot_pct': 0, 'settle_s': 200, 'reversals': 0, 'saturated': False}, 10)
        self.assertEqual(f, at.TRIM_UP)
        f, _ = at.trim_rule({'overshoot_pct': 0, 'settle_s': None, 'reversals': 0, 'saturated': True}, 10)
        self.assertEqual(f, 1.0)     # the valve at an end: not the gains
        f, _ = at.trim_rule({'overshoot_pct': 4, 'settle_s': 50, 'reversals': 1, 'saturated': False}, 10)
        self.assertEqual(f, 1.0)

    def test_step_figures(self):
        rows = [(t, 5 + (1 - math.exp(-t / 5)) + (0.2 * math.exp(-t / 10) * math.sin(t / 3) if t > 5 else 0), 4.0)
                for t in range(0, 200)]
        f = at.step_figures(rows, 5.0, 6.0, 0.05)
        self.assertGreater(f['overshoot_pct'], 0)
        self.assertIsNotNone(f['settle_s'])
        self.assertFalse(f['saturated'])
        # A down step that undershoots once and settles: one reversal at most.
        rows = [(t, 6 - (1 - math.exp(-t / 4)) * (1 + 0.15 * math.exp(-t / 15) * math.cos(t / 4)), 4.0)
                for t in range(0, 200)]
        f = at.step_figures(rows, 6.0, 5.0, 0.05)
        self.assertLessEqual(f['reversals'], 2)

    def test_sets(self):
        rows = [{'sp_mbar': s, 'kp': kp, 'ti_min': ti} for s, kp, ti in
                [(1, 0.05, 0.4), (2, 0.08, 0.4), (4, 0.2, 0.4), (8, 0.5, 0.3), (12, 0.8, 0.3), (16, 0.8, 0.3),
                 (24, 1.0, 0.3), (48, 1.44, 0.3)]]
        sets = at.table_sets(rows)
        self.assertEqual([s[3] for s in sets], [[1, 2, 4], [8, 12, 16], [24, 48]])
        self.assertAlmostEqual(sets[0][1], round((0.05 * 0.08 * 0.2) ** (1 / 3), 2))
        self.assertAlmostEqual(sets[2][1], round(math.sqrt(1.44), 2))
        self.assertEqual(at.write_lines(sets),
                         ['PIDS SET 1,0.09,0.40,0', 'PIDS SET 2,0.68,0.30,0', 'PIDS SET 3,1.20,0.30,0',
                          'SCHED SET PV,6.0,20.0,15'])
        # A band with no point: left as it is.
        sets = at.table_sets(rows[3:])
        self.assertEqual(sets[0][1:], (None, None, []))
        self.assertEqual(len(at.write_lines(sets)), 3)

    def test_put_back(self):
        f = {'sp': 8.0, 'mode': 'AUTO', 'valve': 4.2, 'pid': [0.1, 0.4, 0.0], 'sched': 'PV,6.0,20.0,15',
             'set1': [0.1, 0.4, 0.0]}
        self.assertEqual(at.put_back_lines(f), ['PIDS SET 1,0.10,0.40,0.00', 'SCHED SET PV,6.0,20.0,15',
                                                'PRES SP 8.0', 'MODE AUTO'])
        f.update(mode='MANUAL', sched=None)
        self.assertEqual(at.put_back_lines(f), ['PID SET 0.10,0.40,0.00', 'PRES SP 8.0', 'MODE MANUAL',
                                                'VALV SET 4.2'])


class FakeBoard:
    """The text port on the model: the firmware's PI (1 s, clamp anti-windup,
    bumpless), PID set 1 to 3 and the PID table, as pmc_gains.Link."""

    def __init__(self, plant, sched=True):
        self.P, self.sched = plant, sched
        self.mode, self.sp = 'MANUAL', 5.0
        self.sets = [[4.0, 0.25, 0.0], [4.0, 0.25, 0.0], [4.0, 0.25, 0.0]]
        self.table = 'OFF,6.0,20.0,15'
        self.i = 0.0
        self.t_loop = 0.0
        self.log = []

    # the board's own time: the plant runs and AUTO steps once a second
    def advance(self, t):
        while self.t_loop + 1.0 <= t + 1e-9:
            self.t_loop += 1.0
            self.P.run_to(self.t_loop)
            if self.mode == 'AUTO':
                kp, ti, _ = self.sets[0]
                e = self.sp - self.P.p()
                self.i += kp / (60 * ti) * e
                u = kp * e + self.i
                if u > 100:
                    u, self.i = 100.0, 100 - kp * e
                if u < 0:
                    u, self.i = 0.0, -kp * e
                self.P.cmd = u
        self.P.run_to(t)

    def ask(self, line):
        out = []
        for c in line.split(';'):
            c = c.strip()
            r = self.one(c)
            if r is None:
                continue
            if r.startswith('ERR'):
                raise g.Refused(r)
            out.append(r)
        self.log.append(line)
        return out

    def do(self, cmd):
        return self.ask(cmd + ';MODE?')

    def one(self, c):
        P = self.P
        if c == 'PRES?':
            return 'PRES %.3f mbar' % P.p()
        if c == 'PRES SP?':
            return 'PRES SP %.1f mbar' % self.sp
        if c == 'VALV?':
            return 'VALV %.2f %%' % P.v
        if c == 'VALV MOVING?':
            return 'VALV MOVING %d' % (abs(P.cmd - P.v) > 1e-6)
        if c == 'MODE?':
            return 'MODE ' + self.mode
        if c == 'PID?':
            return 'PID %.2f,%.2f,%.2f' % tuple(self.sets[0])
        if c == 'SCHED?':
            return 'SCHED ' + self.table if self.sched else 'ERR 100 Unknown command'
        if c == 'PIDS?':
            return ('PIDS 1,' + ','.join('%.2f' % v for s in self.sets for v in s)) if self.sched else 'ERR 100'
        if c.startswith('PRES SP '):
            self.sp = float(c[8:])
            return None
        if c == 'MODE AUTO':
            if self.mode == 'MANUAL' and abs(P.cmd - P.v) > 1e-6:
                return 'ERR 212 Wait for the valve to stop'
            if self.mode != 'AUTO':
                self.i = P.v - self.sets[0][0] * (self.sp - P.p())
            self.mode = 'AUTO'
            return None
        if c == 'MODE MANUAL':
            self.mode = 'MANUAL'
            P.cmd = P.v
            return None
        if c.startswith('VALV SET '):
            if self.mode != 'MANUAL':
                return 'ERR 211 not in MANUAL'
            P.cmd = float(c[9:])
            return None
        if c.startswith('PID SET '):
            if self.mode == 'MANUAL' and abs(P.cmd - P.v) > 1e-6:
                return 'ERR 212 wait for the valve to stop'
            v = [float(x) for x in c[8:].split(',')]
            e = self.sp - P.p()
            self.i += (self.sets[0][0] - v[0]) * e      # bumpless
            self.sets[0][:len(v)] = v
            return None
        if c.startswith('PIDS SET '):
            v = c[9:].split(',')
            self.sets[int(v[0]) - 1][:len(v) - 1] = [float(x) for x in v[1:]]
            return None
        if c.startswith('SCHED SET '):
            self.table = c[10:]
            return None
        return 'ERR 100 Unknown command %s' % c


class Clock:
    """Board seconds, run on the model instead of waiting."""

    def __init__(self, board):
        self.b, self.t = board, 0.0

    def now(self):
        return self.t

    def wait_until(self, t):
        if t > self.t:
            self.t = t
            self.b.advance(t)


class Sequence(unittest.TestCase):
    def test_two_points_and_put_back(self):
        # Gain falls with pressure, as on the cryostat: 4 mbar/% low, 1 high.
        curve = lambda v: 0.67 + 30 * (1 - math.exp(-max(v, 0) / 12))   # noqa: E731
        plant = Plant(curve=curve, L=2.0, tau=8.0, noise=0.01)
        b = FakeBoard(plant)
        b.advance(200)
        f = at.found(b)
        self.assertEqual(f['sched'], 'OFF,6.0,20.0,15')
        a = SimpleNamespace(div=4.0, ti_pu=2.0, trims=2, d=0.4)
        d = tempfile.mkdtemp()
        t = at.AutoTuner(b, Clock(b), a, d)
        t.say = lambda m: None
        rows, ku, gains = [], None, (0.30, 0.40)
        for sp in [4, 12]:
            r = t.point(sp, gains, at.next_d(sp, ku, a.d))
            rows.append(r)
            ku, gains = r['Ku'], (r['kp'], r['ti_min'])
        for r in rows:
            K = 30 / 12 * math.exp(-(-12 * math.log(1 - (r['sp_mbar'] - 0.67) / 30)) / 12)   # local mbar/%
            # Ku should be near the plant's own, so KP lands near Ku/4.
            self.assertGreater(r['Ku'], 1.5 / K)
            self.assertLess(r['Ku'], 6.0 / K)
            self.assertGreater(r['kp'], 0)
        self.assertGreater(rows[1]['kp'], rows[0]['kp'])   # lower gain at 12 mbar: more KP
        self.assertTrue(os.path.exists(os.path.join(d, 'relay_4mbar.csv')))
        self.assertTrue(os.path.exists(os.path.join(d, 'check_12mbar_1_up.csv')))
        for line in at.put_back_lines(f):
            b.do(line)
        self.assertEqual((b.mode, b.sets[0], b.table), ('MANUAL', [4.0, 0.25, 0.0], 'OFF,6.0,20.0,15'))


if __name__ == '__main__':
    unittest.main()
