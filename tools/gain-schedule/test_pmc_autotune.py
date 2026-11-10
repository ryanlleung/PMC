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

    def test_smaller_step_then_fails_when_the_pressure_runs_away(self):
        R = at.Relay(5.0, 5.0, 0.4, 0.05)
        R.feed(0, 5.0, 5.0)
        # Too far: the step is cut to a quarter (the least), the valve below x0.
        self.assertEqual(R.feed(1, 11.0, 5.0), 4.9)
        self.assertFalse(R.done)
        self.assertEqual(R.d, at.D_MIN)
        # Still far the next second (9 Nov, 2 mbar): it waits for the pressure.
        self.assertIsNone(R.feed(2, 11.0, 5.0))
        self.assertFalse(R.done)
        # Back: the test goes on; far again with the least step: it gives up.
        R.feed(30, 5.2, 4.9)
        self.assertFalse(R.done)
        R.feed(31, 11.0, 4.9)
        self.assertTrue(R.done)
        self.assertIn('too far', R.why)

    def test_gives_up_when_the_pressure_does_not_come_back(self):
        R = at.Relay(5.0, 5.0, 0.4, 0.05)
        R.feed(0, 5.0, 5.0)
        R.feed(1, 11.0, 5.0)
        for t in range(2, 2 + at.RUNOFF_S):
            R.feed(t, 11.0, 4.9)
        self.assertFalse(R.done)
        R.feed(2 + at.RUNOFF_S, 11.0, 4.9)
        self.assertTrue(R.done)
        self.assertIn('not back', R.why)


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


class Watchdog(unittest.TestCase):
    def feed(self, sp, ps):
        w = at.Wild(sp)
        for p in ps:
            if w.feed(p):
                break
        return w

    def test_trips_on_ryans_swing(self):
        # 9 Nov, 2 mbar reach with the 1 mbar gains: 1 to 10 mbar every 14 s.
        ps = [1.0 + 4.5 * (1 - math.cos(2 * math.pi * t / 14)) for t in range(60)]
        w = self.feed(2.0, ps)
        self.assertTrue(w.wild)
        self.assertEqual(w.cut(), at.WILD_CUT_BIG)    # swing larger than the setpoint

    def test_trips_when_the_floor_cuts_the_low_side(self):
        # 1 mbar, the pump's floor 0.93: only the top swings.
        ps = [max(0.93, 1.0 + 0.6 * math.sin(2 * math.pi * t / 12)) for t in range(60)]
        w = self.feed(1.0, ps)
        self.assertTrue(w.wild)
        self.assertEqual(w.cut(), at.WILD_CUT)

    def test_quiet_on_a_step_that_rings_down(self):
        # 10 to 12 mbar, 40 % overshoot, ringing down within a few cycles.
        ps = [12 - 2 * math.exp(-t / 6) * math.cos(t / 3) for t in range(200)]
        self.assertFalse(self.feed(12.0, ps).wild)

    def test_quiet_on_the_needle_limit_cycle(self):
        # 10 mbar, 0.4 mbar peak to peak (stick-slip, 4 Nov): not a reason to cut.
        ps = [10 + 0.2 * math.sin(2 * math.pi * t / 40) for t in range(600)]
        self.assertFalse(self.feed(10.0, ps).wild)

    def test_trim_rule_cuts_harder_than_overshoot(self):
        f, why = at.trim_rule({'overshoot_pct': 0, 'settle_s': None, 'reversals': 9, 'saturated': False,
                               'wild_pp': 3.0, 'wild_big': True}, 10)
        self.assertEqual(f, at.WILD_CUT_BIG)
        self.assertIn('oscillating', why)


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
        for sp in (4, 12):   # only the check each result came from is kept
            ups = [n for n in os.listdir(d) if n.startswith('check_%gmbar_' % sp) and n.endswith('_up.csv')]
            self.assertEqual(len(ups), 1, ups)
        for line in at.put_back_lines(f):
            b.do(line)
        self.assertEqual((b.mode, b.sets[0], b.table), ('MANUAL', [4.0, 0.25, 0.0], 'OFF,6.0,20.0,15'))


class SteepValve(unittest.TestCase):
    """Ryan's valve on 9 Nov: almost no flow change below about 3.1 %, then
    about 18 mbar per % (3.0 % gave 1 mbar, 3.6 % 10 mbar)."""

    @staticmethod
    def plant():
        curve = lambda v: 0.67 + 0.12 * max(v, 0) + 18 * max(v - 3.1, 0)   # noqa: E731
        p = Plant(curve=curve, L=2.0, tau=3.0, noise=0.005)
        p.v = p.cmd = p.eff = 3.0
        p.x = curve(3.0)
        return p

    def tuner(self, b):
        a = SimpleNamespace(div=4.0, ti_pu=2.0, trims=2, d=0.4)
        t = at.AutoTuner(b, Clock(b), a, None)
        t.say = lambda m: None
        t.cuts = []
        return t

    def test_reach_cuts_kp_until_it_holds(self):
        # The 1 mbar result (KP about 3) used to reach 2 mbar: it swung 1 to
        # 10 mbar. The watchdog cuts KP until AUTO holds.
        b = FakeBoard(self.plant())
        b.advance(30)
        t = self.tuner(b)
        x0 = t.reach(2.0, 3.0, 0.40)
        self.assertTrue(t.cuts)
        self.assertTrue(all(c.endswith('osc') for c in t.cuts))
        self.assertLess(b.sets[0][0], 0.5)
        self.assertGreater(x0, 3.0)
        self.assertLess(x0, 3.3)
        self.assertAlmostEqual(b.P.p(), 2.0, delta=0.3)

    def test_a_cut_starts_the_stage_again(self):
        # Ryan 10 Nov: "12 min of at most 10 min". Each cut starts the wait
        # again; the stage line (its clock, and the KP it shows) does too.
        b = FakeBoard(self.plant())
        b.advance(30)
        t = self.tuner(b)
        stages = []
        t.stage = lambda text, phase=None, most=None: stages.append((text, phase, most))
        t.reach(2.0, 3.0, 0.40)
        self.assertEqual(len(stages), len(t.cuts) + 1)
        self.assertEqual(stages[0], ('AUTO at 2 mbar with KP 3.00 TI 0.40, waiting for it to hold', '2 mbar reach',
                                     at.REACH_MAX_S))
        for text, phase, most in stages[1:]:
            self.assertIn('(cut from 3.00)', text)
            self.assertEqual((phase, most), (None, at.REACH_MAX_S))
        self.assertIn('KP %.2f ' % b.sets[0][0], stages[-1][0])

    def test_point_ends_with_gains_that_do_not_oscillate(self):
        # Less steep above the knee (6 mbar per %), so the relay at 2 mbar
        # works with a 0.1 % step; the reach with KP 3 still swings wildly.
        curve = lambda v: 0.67 + 0.12 * max(v, 0) + 6 * max(v - 3.1, 0)   # noqa: E731
        p = Plant(curve=curve, L=2.0, tau=3.0, noise=0.005)
        p.v = p.cmd = p.eff = 3.0
        p.x = curve(3.0)
        b = FakeBoard(p)
        b.advance(30)
        t = self.tuner(b)
        r = t.point(2.0, (3.0, 0.40), 0.4)
        # The check's figures with the gains it kept: no oscillation.
        self.assertFalse(r['note'].startswith('still'), r)
        self.assertLess(r['kp'], 0.5)
        self.assertIn('reach oscillated', r['note'])


class Wander(unittest.TestCase):
    def test_half_the_swing_about_the_drift(self):
        rows = [(t, 4.0 + 0.01 * t + 0.15 * math.sin(t / 3.0)) for t in range(40)]
        self.assertAlmostEqual(at.wander(rows), 0.15, delta=0.02)

    def test_overshoot_counts_beyond_the_wander(self):
        rows = [(0, 4.0, 3.2), (5, 4.6, 3.3), (10, 5.0, 3.3), (20, 4.8, 3.3)] + [(t, 4.8, 3.3) for t in range(21, 100)]
        self.assertEqual(at.step_figures(rows, 4.0, 4.8, 0.04)['overshoot_pct'], 25.0)
        self.assertEqual(at.step_figures(rows, 4.0, 4.8, 0.17, 0.17)['overshoot_pct'], 3.8)

    def tuner(self, figs):
        a = SimpleNamespace(div=4.0, ti_pu=2.0, trims=2, d=0.4)
        t = at.AutoTuner(None, None, a, None)
        t.say = lambda m: None
        t.reach = lambda sp, kp, ti, why=None: 3.0
        t.relay = lambda sp, x0, dd: {'Ku': 0.68, 'Pu_s': 8.2, 'd_pct': 0.2, 'a_mbar': 0.33, 'eps_mbar': 0.1,
                                      'cycles': 4, 'asym': None, 'steady': True, 'noise': 0.03, 'wander': 0.17}
        t.check = lambda sp, kp, ti, r, k: (figs[k - 1], kp)
        return t

    def test_cuts_stop_when_two_do_not_help(self):
        # 4 mbar on 9 Nov: the check no better after each cut.
        fig = lambda o, n: {'overshoot_pct': o, 'settle_s': None, 'reversals': n, 'saturated': False}   # noqa: E731
        t = self.tuner([fig(24.5, 9), fig(65.9, 2), fig(32.4, 12), fig(29.2, 4)])
        r = t.point(4.0, (0.1, 0.4), 0.4)
        self.assertEqual(r['trims'], 'x0.80 x0.80')
        self.assertAlmostEqual(r['kp'], at.clip_kp(0.17 * 0.64), places=2)
        self.assertIn('cuts stopped', r['note'])

    def test_cuts_go_on_while_they_help(self):
        fig = lambda o, n: {'overshoot_pct': o, 'settle_s': 30, 'reversals': n, 'saturated': False}   # noqa: E731
        t = self.tuner([fig(40, 1), fig(30, 1), fig(20, 1), fig(8, 1)])
        r = t.point(4.0, (0.1, 0.4), 0.4)
        self.assertEqual(r['trims'], 'x0.80 x0.80 x0.80')
        self.assertEqual(r['note'], '')


class Files(unittest.TestCase):
    def test_a_check_that_misses_spec_is_deleted_when_redone(self):
        d = tempfile.mkdtemp()
        a = SimpleNamespace(div=4.0, ti_pu=2.0, trims=2, d=0.4)
        t = at.AutoTuner(None, None, a, d)
        t.say = lambda m: None
        t.reach = lambda sp, kp, ti, why=None: 3.0
        t.relay = lambda sp, x0, dd: {'Ku': 2.0, 'Pu_s': 10.0, 'd_pct': 0.4, 'a_mbar': 0.1, 'eps_mbar': 0.02,
                                      'cycles': 2, 'asym': None, 'steady': True, 'noise': 0.004}
        figs = [{'overshoot_pct': 30.0, 'settle_s': 40, 'reversals': 1, 'saturated': False},
                {'overshoot_pct': 15.0, 'settle_s': 40, 'reversals': 1, 'saturated': False},
                {'overshoot_pct': 5.0, 'settle_s': 40, 'reversals': 1, 'saturated': False}]

        def check(sp, kp, ti, r, k):
            for leg in ('up', 'back'):
                open(os.path.join(d, 'check_%gmbar_%d_%s.csv' % (sp, k, leg)), 'w').close()
            return figs[k - 1], kp
        t.check = check
        r = t.point(2.0, (0.1, 0.4), 0.4)
        self.assertEqual(r['trims'], 'x0.80 x0.80')
        self.assertEqual(sorted(os.listdir(d)), ['check_2mbar_3_back.csv', 'check_2mbar_3_up.csv'])
        self.assertIn('Check 1 missed spec: check_2mbar_1_up.csv, check_2mbar_1_back.csv deleted', t.calcs)



class Learned(unittest.TestCase):
    """Ryan, 9 Nov: refine the numbers from run to run, not start fresh."""
    def tuner(self, ku):
        a = SimpleNamespace(div=4.0, ti_pu=2.0, trims=2, d=0.4)
        t = at.AutoTuner(None, None, a, None)
        t.say = lambda m: None
        t.reach = lambda sp, kp, ti, why=None: 7.0
        t.relay = lambda sp, x0, dd: {'Ku': ku, 'Pu_s': 18.0, 'd_pct': 0.8, 'a_mbar': 0.95, 'eps_mbar': 0.05,
                                      'cycles': 2, 'asym': None, 'steady': True, 'noise': 0.01}
        self.used = []

        def check(sp, kp, ti, r, k):
            self.used.append((kp, ti))
            return {'overshoot_pct': 0.0, 'settle_s': 60, 'reversals': 0, 'saturated': False}, kp
        t.check = check
        return t

    def test_file_round_trip(self):
        d = tempfile.mkdtemp()
        path = os.path.join(d, at.LEARNED_FILE)
        self.assertEqual(at.load_learned(path), {})
        L = {}
        at.learn(L, {'sp_mbar': 16.0, 'kp': 0.42, 'ti_min': 0.6, 'Ku': 1.07, 'Pu_s': 18.0, 'd_pct': 0.8,
                     'a_mbar': 0.95, 'valve_pct': 6.8, 'trims': 'x1.25 x1.25', 'note': ''}, '09/11/26 16:30')
        at.learn(L, {'sp_mbar': 4.0, 'kp': 0.3, 'ti_min': 0.5, 'Ku': 2.0, 'Pu_s': 15.0}, '09/11/26 16:00')
        at.save_learned(path, L)
        back = at.load_learned(path)
        self.assertEqual(sorted(back), ['16', '4'])
        self.assertEqual((back['16']['kp'], back['16']['ti_min'], back['16']['Ku']), (0.42, 0.6, 1.07))
        self.assertEqual(back['16']['trims'], 'x1.25 x1.25')
        with open(path) as f:
            self.assertTrue(f.readlines()[1].startswith('4,'))   # lowest setpoint first

    def test_seeded_from_earlier_runs(self):
        d = tempfile.mkdtemp()
        cols = 'time,sp_mbar,kp,ti_min,Ku,Pu_s,kp_rule,note\n'
        runs = {'autotune_20261109_150000': cols + '2026-11-09 15:10:00,16.0,0.30,0.50,1.0,17,0.27,\n'
                                              '2026-11-09 15:20:00,24.0,,,,,,no relay cycles\n',
                'autotune_20261109_160000': cols + '2026-11-09 16:30:00,16.0,0.42,0.60,1.07,18,0.27,\n'
                                              '2026-11-09 16:45:00,24.0,0.43,0.67,1.70,20,0.43,\n'}
        for name, text in runs.items():
            os.mkdir(os.path.join(d, name))
            with open(os.path.join(d, name, 'results.csv'), 'w') as f:
                f.write(text)
        L = at.seed_learned(d)
        self.assertEqual(sorted(L), ['16', '24'])
        self.assertEqual((L['16']['kp'], L['16']['ti_min'], L['16']['Ku']), (0.42, 0.6, 1.07))   # newest
        self.assertEqual(L['16']['when'], '09/11/26 16:30')
        self.assertEqual(L['24']['kp'], 0.43)       # the failed 24 mbar row is skipped
        self.assertEqual(at.seed_learned(os.path.join(d, 'none')), {})

    def test_analysed_gains_go_in_once_over_older_rows(self):
        d = tempfile.mkdtemp()
        path = os.path.join(d, 'analysed.csv')
        with open(path, 'w') as f:
            f.write(','.join(at.LEARNED_COLS) + '\n'
                    '1,0.10,0.80,7.46,12.5,,,2.7,,analysed,10/11/26 12:15\n'
                    '16,0.42,0.60,1.07,18.0,0.8,1.05,6.8,x1.25 x1.25,analysed,10/11/26 12:15\n'
                    '24,0.43,0.67,1.70,20.0,,,,,analysed,10/11/26 12:15\n')
        L = {}
        at.learn(L, {'sp_mbar': 1.0, 'kp': 1.2, 'ti_min': 0.42, 'Ku': 7.46}, '09/11/26 14:00')   # older
        at.learn(L, {'sp_mbar': 16.0, 'kp': 0.5, 'ti_min': 0.7}, '11/11/26 09:00')               # a later run
        at.learn(L, {'sp_mbar': 8.0, 'kp': 0.1, 'ti_min': 0.83}, '09/11/26 14:00')               # not analysed
        self.assertEqual(sorted(at.merge_analysed(L, path)), ['1', '24'])
        self.assertEqual((L['1']['kp'], L['1']['ti_min'], L['1']['when']), (0.1, 0.8, '10/11/26 12:15'))
        self.assertEqual(L['16']['kp'], 0.5)        # the later autotune result wins
        self.assertEqual(L['8']['kp'], 0.1)
        self.assertEqual(L['24']['Ku'], 1.7)
        self.assertEqual(at.merge_analysed(L, path), [])   # once only
        self.assertEqual(at.merge_analysed(L, os.path.join(d, 'none.csv')), [])

    def test_the_shipped_analysed_table_reads(self):
        L = {}
        at.merge_analysed(L)
        self.assertEqual(sorted(L, key=float), ['1', '2', '4', '8', '12', '16', '24'])
        for k, r in L.items():
            self.assertTrue(g.KP_MIN <= r['kp'] <= g.KP_MAX and g.TI_MIN <= r['ti_min'] <= g.TI_MAX, k)
        self.assertEqual((L['1']['kp'], L['16']['kp'], L['16']['ti_min']), (0.1, 0.42, 0.6))

    def test_check_starts_from_the_learned_gains(self):
        t = self.tuner(1.2)   # within 2 x of the learned 1.07
        prior = {'sp_mbar': 16.0, 'kp': 0.42, 'ti_min': 0.6, 'Ku': 1.07, 'Pu_s': 18.0, 'trims': 'x1.25 x1.25'}
        r = t.point(16.0, (0.42, 0.6), 0.8, prior)
        self.assertEqual(self.used[0], (0.42, 0.6))           # not the rule's 1.2 / 4 = 0.30
        self.assertEqual(r['kp_rule'], 0.3)
        self.assertIn('refined from learned KP 0.42 TI 0.60', r['note'])
        self.assertTrue(any(c.startswith('Refine:') for c in t.calcs))

    def test_fresh_when_the_plant_has_changed(self):
        t = self.tuner(3.3)   # 3 x the learned Ku
        prior = {'sp_mbar': 16.0, 'kp': 0.42, 'ti_min': 0.6, 'Ku': 1.07, 'Pu_s': 18.0}
        r = t.point(16.0, (0.42, 0.6), 0.8, prior)
        self.assertEqual(self.used[0], (round(3.3 / 4, 2), 0.6))
        self.assertNotIn('refined', r['note'])
        self.assertTrue(any(c.startswith('Fresh:') and 'the plant has changed' in c for c in t.calcs))

    def test_no_prior_is_the_old_behaviour(self):
        t = self.tuner(1.2)
        t.point(16.0, (0.1, 0.4), 0.8)
        self.assertEqual(self.used[0], (0.3, 0.6))
        self.assertFalse(any(c.startswith(('Refine:', 'Fresh:', 'Learned')) for c in t.calcs))


if __name__ == '__main__':
    unittest.main()
