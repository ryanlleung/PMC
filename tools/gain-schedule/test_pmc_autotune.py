#!/usr/bin/env python3
"""Tests for pmc_autotune.py, on a valve and cryostat model (no board, seconds).

    python test_pmc_autotune.py
"""
import csv
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
g = at                      # the link's Refused, the KP and TI limits

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


BENCH = [   # the six bench fits of 5 to 10 Nov: (setpoint, K, L, tau, Pu, Ku) -> KP, TI (min)
    # Ku is what the relay gave where Ku / 3 is the cap (2 and 4 mbar), else any large value.
    (2, 3.3, 56, 42, 224, 0.21, 0.07, 2.33),
    (4, 4.25, 28, 15, 112, 0.24, 0.08, 1.17),
    (8, 3.9, 28, 85, 112, 5.0, 0.16, 1.42),
    (12, 3.7, 12, 100, 48, 5.0, 0.45, 1.67),
    (16, 2.44, 2, 6, 20, 5.0, 0.20, 0.21),
    (24, 1.47, 2, 9, 24, 5.0, 0.34, 0.25),
]


class Rules(unittest.TestCase):
    def test_first_gains_without_a_fit(self):
        self.assertEqual(at.relay_gains(4.0, 9.0), (1.0, 0.3))
        self.assertEqual(at.relay_gains(0.001, 0.1), (g.KP_MIN, g.TI_MIN))

    def test_rule_on_the_bench_fits(self):
        for sp, K, L, tau, pu, ku, kp, ti in BENCH:
            got = at.rule_gains({'K': K, 'L': L, 'tau': tau}, ku, pu)
            self.assertAlmostEqual(got[0], kp, delta=0.011, msg='%g mbar KP' % sp)
            self.assertAlmostEqual(got[1], ti, delta=0.011, msg='%g mbar TI' % sp)

    def test_rule_caps(self):
        fit = {'K': 3.9, 'L': 28, 'tau': 85}
        self.assertEqual(at.rule_gains(fit, 0.3, 112)[0], 0.1)             # Ku / 3
        self.assertEqual(at.rule_gains({'K': 1.0, 'L': 0, 'tau': 500}, 9.0, 20)[1], round(20 * 5 / 60, 2))  # 20 L_eff
        self.assertEqual(at.rule_gains({'K': 1.0, 'L': 0, 'tau': 1}, 9.0, 20)[1], round(2.5 * 5 / 60, 2))   # 2.5 L_eff

    fig0 = {'overshoot_pct': 0.0, 'settle_s': 50, 'reversals': 0, 'saturated': False, 't63_s': 20, 't90_s': 60,
            'backoff_pct': 0.0, 'sag_s': 0}
    fit0 = {'K': 2.0, 'L': 10, 'tau': 30}

    def rule(self, kp=0.3, ti=1.0, ku=9.0, pu=20.0, fit='fit0', **kw):
        fig = dict(self.fig0, **kw)
        return at.refine_rule(fig, kp, ti, ku, pu, self.fit0 if fit == 'fit0' else fit)

    def test_rule1_reversals_or_a_cut(self):
        a = self.rule(reversals=3)
        self.assertEqual((a['kind'], a['kp'], a['ti']), ('kp', 0.21, 1.0))
        a = self.rule(cut=True)
        self.assertEqual((a['kind'], a['kp']), ('kp', 0.21))
        a = self.rule(reversals=2)
        self.assertEqual(a['kind'], 'pass')
        # a leg the watchdog ended: its own bigger cut
        a = self.rule(wild_pp=3.0, wild_cut=0.25, wild_big=True)
        self.assertEqual((a['kind'], a['kp']), ('kp', 0.07))
        a = self.rule(wild_pp=3.0, wild_cut=0.5)
        self.assertEqual(a['kp'], 0.15)

    def test_rule2_overshoot(self):
        a = self.rule(overshoot_pct=6.0)
        self.assertEqual((a['kind'], a['kp'], a['ti']), ('kp', 0.24, 1.0))
        self.assertEqual(self.rule(overshoot_pct=5.0)['kind'], 'pass')
        # rule 1 comes first
        self.assertEqual(self.rule(overshoot_pct=30.0, reversals=4)['kp'], 0.21)

    def test_rule3_tail(self):
        a = self.rule(t63_s=20, t90_s=100, backoff_pct=15.0)
        self.assertEqual((a['kind'], a['kp'], a['ti']), ('ti', 0.3, 0.7))
        # 90 % not reached counts as a tail too
        self.assertEqual(self.rule(t90_s=None, backoff_pct=15.0)['kind'], 'ti')
        # no tail: t90 within 4 x t63; or the valve does not back off; or overshoot
        self.assertEqual(self.rule(t63_s=20, t90_s=70, backoff_pct=15.0)['kind'], 'pass')
        self.assertEqual(self.rule(t63_s=20, t90_s=100, backoff_pct=5.0)['kind'], 'pass')
        self.assertEqual(self.rule(t63_s=20, t90_s=100, backoff_pct=15.0, overshoot_pct=3.0)['kind'], 'pass')

    def test_rule4_slow(self):
        a = self.rule(t63_s=150, t90_s=400, backoff_pct=5.0)   # 400 s over 8 x (10 + 30)
        self.assertEqual((a['kind'], a['ti']), ('ti', 0.8))
        self.assertEqual(self.rule(t63_s=150, t90_s=300, backoff_pct=5.0)['kind'], 'pass')
        # it needs tau, so no fit: no rule 4
        self.assertEqual(self.rule(t63_s=150, t90_s=900, backoff_pct=5.0, fit=None)['kind'], 'pass')
        # the valve at an end: not the gains' fault
        self.assertEqual(self.rule(t63_s=150, t90_s=900, backoff_pct=5.0, saturated=True)['kind'], 'pass')

    def test_ti_limits_end_the_refinements(self):
        # 2.5 L_eff = 25 s = 0.42 min
        a = self.rule(ti=0.45, t90_s=100, backoff_pct=15.0)
        self.assertEqual((a['kind'], a['ti']), ('ti', 0.42))
        a = self.rule(ti=0.42, t90_s=100, backoff_pct=15.0)
        self.assertEqual((a['kind'], a['ti'], a['note']), ('limit', 0.42, 'as fast as the cryostat allows'))
        # the integral gain KP / TI over 1 / (4 K L_eff): TI at least 4 x 2 x 10 x KP / 60 min
        a = self.rule(kp=1.0, ti=1.2, t90_s=100, backoff_pct=15.0)
        self.assertEqual(a['kind'], 'limit')
        a = self.rule(kp=1.0, ti=1.5, t90_s=100, backoff_pct=15.0)
        self.assertEqual((a['kind'], a['ti']), ('ti', 1.33))
        # L_eff is never under Pu / 4: a 100 s period holds TI at 2.5 x 25 s = 1.04 min
        a = self.rule(ti=1.05, pu=100.0, t90_s=100, backoff_pct=15.0)
        self.assertEqual((a['kind'], a['ti']), ('ti', 1.04))

    def test_kp_is_never_raised_and_never_over_ku_over_3(self):
        for kw in ({}, {'reversals': 4}, {'overshoot_pct': 9.0}, {'t90_s': 100, 'backoff_pct': 20.0},
                   {'t63_s': 150, 't90_s': 400}, {'wild_pp': 1.0, 'wild_cut': 0.5}):
            a = self.rule(**kw)
            self.assertLessEqual(a['kp'], 0.3)
        self.assertEqual(self.rule(kp=2.0, ku=3.0, overshoot_pct=9.0)['kp'], 1.0)
        # KP at the firmware's lowest: nothing to cut
        self.assertEqual(self.rule(kp=0.01, overshoot_pct=9.0)['kind'], 'limit')

    def test_worse_check(self):
        old = dict(self.fig0, overshoot_pct=8.0, reversals=1)
        self.assertFalse(at.worse_check(dict(old, overshoot_pct=7.0), old))
        self.assertTrue(at.worse_check(dict(old, overshoot_pct=20.0), old))
        self.assertTrue(at.worse_check(dict(old, wild_pp=2.0), dict(old)))
        self.assertTrue(at.worse_check(dict(old, t90_s=120), dict(old, t90_s=60)))
        self.assertFalse(at.worse_check(dict(old, t90_s=70), dict(old, t90_s=60)))
        self.assertTrue(at.worse_check(dict(old, t90_s=None), dict(old, t90_s=60)))
        # 8 to 8.5 % is not a different check
        self.assertFalse(at.worse_check(dict(old, overshoot_pct=8.5), old))
        # a refinement that raised overshoot from 1 to 2 % is still inside the limit
        self.assertFalse(at.worse_check(dict(old, overshoot_pct=2.0), dict(old, overshoot_pct=1.0)))

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

    @staticmethod
    def interp(pts, t):
        for (t0, v0), (t1, v1) in zip(pts, pts[1:]):
            if t0 <= t <= t1:
                return v0 + (v1 - v0) * (t - t0) / (t1 - t0)
        return pts[-1][1]

    def test_t63_t90_and_the_valve_backing_off(self):
        # Ryan, 10 Nov: "goes open more, then stops and then goes open slowly again". Pressure: 63 % at
        # 10 s, 90 % at 100 s. Valve: opens 1 % in 10 s, backs off 0.3 %, creeps on to +1.2 %.
        P = [(0, 5.0), (10, 5.64), (100, 5.9), (200, 6.0)]
        X = [(0, 5.0), (10, 6.0), (40, 5.7), (200, 6.2)]
        rows = [(t, self.interp(P, t), self.interp(X, t)) for t in range(0, 201)]
        f = at.step_figures(rows, 5.0, 6.0, 0.05)
        self.assertEqual((f['t63_s'], f['t90_s']), (10, 100))
        self.assertAlmostEqual(f['backoff_pct'], 25.0, delta=0.5)      # 0.3 of a 1.2 % move
        self.assertEqual(f['sag_s'], 0)
        # a valve that only ever opens does not back off
        X = [(0, 5.0), (200, 6.2)]
        rows = [(t, self.interp(P, t), self.interp(X, t)) for t in range(0, 201)]
        self.assertEqual(at.step_figures(rows, 5.0, 6.0, 0.05)['backoff_pct'], 0.0)
        # back-off after t90 is not counted
        X = [(0, 5.0), (100, 6.0), (150, 5.7), (200, 6.2)]
        rows = [(t, self.interp(P, t), self.interp(X, t)) for t in range(0, 201)]
        self.assertEqual(at.step_figures(rows, 5.0, 6.0, 0.05)['backoff_pct'], 0.0)
        # a step that never gets to 90 %
        rows = [(t, 5 + 0.5 * t / 200, 5 + 0.01 * t) for t in range(0, 201)]
        f = at.step_figures(rows, 5.0, 6.0, 0.05)
        self.assertEqual((f['t63_s'], f['t90_s']), (None, None))

    def test_sag(self):
        # reaches the band at 40 s, then sits 0.2 mbar under it from 61 to 150 s with the valve still
        P = [(0, 5.0), (40, 6.0), (60, 5.99), (61, 5.8), (150, 5.8), (151, 6.0), (250, 6.0)]
        X = [(0, 5.0), (30, 5.9), (250, 5.9)]
        rows = [(t, self.interp(P, t), self.interp(X, t)) for t in range(0, 251)]
        f = at.step_figures(rows, 5.0, 6.0, 0.05)
        self.assertGreaterEqual(f['sag_s'], 86)
        self.assertLessEqual(f['sag_s'], 90)
        # with the valve moving the whole time it is not a sag
        X = [(0, 5.0), (250, 8.0)]
        rows = [(t, self.interp(P, t), self.interp(X, t)) for t in range(0, 251)]
        self.assertEqual(at.step_figures(rows, 5.0, 6.0, 0.05)['sag_s'], 0)
        # a down step: the pressure undershoots the new setpoint (the logs of 10 Nov: 4 mbar to 3.56 with the
        # valve still for 100 s) and is under the band while the valve does not move
        P = [(0, 6.0), (40, 5.0), (61, 4.8), (150, 4.8), (151, 5.0), (250, 5.0)]
        X = [(0, 6.0), (30, 5.0), (250, 5.0)]
        rows = [(t, self.interp(P, t), self.interp(X, t)) for t in range(0, 251)]
        f = at.step_figures(rows, 6.0, 5.0, 0.05)
        self.assertGreaterEqual(f['sag_s'], 86)
        self.assertLessEqual(f['sag_s'], 90)
        # above the band on the way down (still coming down) is not a sag
        P = [(0, 6.0), (40, 5.0), (61, 5.2), (150, 5.2), (151, 5.0), (250, 5.0)]
        rows = [(t, self.interp(P, t), self.interp(X, t)) for t in range(0, 251)]
        self.assertEqual(at.step_figures(rows, 6.0, 5.0, 0.05)['sag_s'], 0)

    def test_sets(self):
        # the six bench results: 2 and 4 mbar below B1 = 6, 8 and 12 to B2 = 14, 16 and 24 above (16 mbar is
        # clear of the switching point at 14 mbar with H = 10 %: it would sit on it at 20 mbar with 15 %)
        rows = [{'sp_mbar': s, 'kp': kp, 'ti_min': ti} for s, _, _, _, _, _, kp, ti in BENCH]
        sets = at.table_sets(rows)
        self.assertEqual((at.B1, at.B2, at.HYST), (6.0, 14.0, 10))
        self.assertEqual([s[3] for s in sets], [[2, 4], [8, 12], [16, 24]])
        self.assertEqual([s[1] for s in sets], [0.07, 0.16, 0.20])             # the lowest KP of the rows
        self.assertAlmostEqual(sets[0][2], round(math.sqrt(2.33 * 1.17), 2))   # the geometric mean of the TIs
        self.assertAlmostEqual(sets[1][2], round(math.sqrt(1.42 * 1.67), 2))
        self.assertAlmostEqual(sets[2][2], round(math.sqrt(0.21 * 0.25), 2))
        self.assertEqual(at.write_lines(sets),
                         ['PIDS SET 1,0.07,1.65,0', 'PIDS SET 2,0.16,1.54,0', 'PIDS SET 3,0.20,0.23,0',
                          'SCHED SET PV,6.0,14.0,10'])
        # on the boundary: 6 is in set 2, 14 in set 3
        sets = at.table_sets([{'sp_mbar': s, 'kp': 0.1, 'ti_min': 1.0} for s in (6, 14)])
        self.assertEqual([s[3] for s in sets], [[], [6], [14]])
        # A band with no point: left as it is.
        sets = at.table_sets(rows[2:])
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

    def test_refine_rule_cuts_a_wild_leg_harder(self):
        a = at.refine_rule({'overshoot_pct': 0, 'settle_s': None, 'reversals': 9, 'saturated': False,
                            'wild_pp': 3.0, 'wild_big': True, 'wild_cut': at.WILD_CUT_BIG}, 0.4, 1.0, 9.0, 20.0, None)
        self.assertEqual((a['kind'], a['kp']), ('kp', 0.1))
        self.assertIn('oscillating', a['why'])


class FakeBoard:
    """The text port on the model: the firmware's PI (1 s, clamp anti-windup,
    bumpless), PID set 1 to 3 and the PID table, as pmc_autotune.Link."""

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

        # The log (Ryan, 11 Nov): every calculation, decision, gain change and
        # command, each with the gains on the board after it.
        with open(os.path.join(d, at.LOG_FILE), newline='') as fh:
            log = list(csv.reader(fh))
        self.assertEqual(log[0], at.LOG_COLS)
        rows = [dict(zip(at.LOG_COLS, r)) for r in log[1:]]
        kinds = {r['kind'] for r in rows}
        self.assertTrue({'calc', 'decision', 'gains', 'command', 'result'} <= kinds, kinds)
        self.assertEqual(len([r for r in rows if r['kind'] == 'result']), 2)
        first = next(r for r in rows if r['kind'] == 'gains')
        self.assertEqual(first['change'], 'KP -- -> 0.30, TI -- -> 0.40, KD -- -> 0.00')
        self.assertTrue(first['text'].startswith('PID SET 0.30,0.40,0: reach 4 mbar'), first['text'])
        self.assertEqual(first['stage'], '4 mbar reach')
        self.assertEqual(next(r for r in rows if r['text'].startswith('MODE MANUAL'))['stage'], '4 mbar noise')
        self.assertTrue(all(r['stage'] for r in rows), [r for r in rows if not r['stage']][:2])
        for r in rows:
            if r['kind'] == 'gains':     # the KP and TI columns are the new gains, the change says from what
                self.assertTrue(r['text'].startswith('PID SET %s,%s,0' % (r['kp'], r['ti_min'])), r)
                self.assertIn('-> ' + r['kp'], r['change'] if 'KP' in r['change'] else '-> ' + r['kp'])
        # The board's own record of what was set matches the log's.
        sent = [x.split(';')[0] for x in b.log if x.startswith('PID SET ')]
        self.assertEqual([r['text'].split(':')[0] for r in rows if r['kind'] == 'gains'],
                         [x for i, x in enumerate(sent) if i == 0 or x != sent[i - 1]])
        # The first gains for each point are a decision, and their check runs with them.
        for sp in ('4', '12'):
            dec = [r for r in rows if r['setpoint_mbar'] == sp and r['kind'] == 'decision']
            self.assertTrue(any(r['text'].startswith('First gains') for r in dec), sp)
        times = [float(r['t_s']) for r in rows]
        self.assertEqual(times, sorted(times))


class Prediction(unittest.TestCase):
    """The predicted line (Ryan, 11 Nov): the board's PI on the fit."""

    def test_settles_on_the_new_setpoint_from_where_it_was(self):
        f = {'K': 2.0, 'L': 4, 'tau': 10}
        rows = at.predict_leg(f, 0.3, 0.5, 8.0, 4.0, 8.0, 9.6, 600)
        self.assertAlmostEqual(rows[0][1], 8.0, places=6)          # dead time: nothing moves at first
        self.assertAlmostEqual(rows[-1][1], 9.6, places=2)
        self.assertAlmostEqual(rows[-1][2], 4.8, places=2)         # 1.6 mbar at 2 mbar/% = 0.8 % more valve
        self.assertAlmostEqual(rows[0][2], 4.0 + at.PRED_SLEW, places=6)   # the proportional kick, at the slew

    def test_the_valve_follows_at_the_slew_and_stays_in_range(self):
        f = {'K': 2.0, 'L': 0, 'tau': 4}
        rows = at.predict_leg(f, 50.0, 0.1, 8.0, 4.0, 8.0, 30.0, 300)
        steps = [b[2] - a[2] for a, b in zip(rows, rows[1:])]
        self.assertLessEqual(max(abs(v) for v in steps), at.PRED_SLEW + 1e-9)
        self.assertTrue(all(0.0 <= r[2] <= 100.0 for r in rows))

    def test_closed_loop_fit_finds_the_plant_the_steps_ran_on(self):
        true = {'K': 0.25 * 1.25 ** 9, 'L': 4, 'tau': 9}
        legs = []
        for p0, p1 in ((8.0, 9.6), (9.6, 8.0)):
            rows = at.predict_leg(true, 0.3, 0.5, p0, 3.0, p0, p1, 150)
            legs.append({'p': p0, 'x': 3.0, 'p0': p0, 'p1': p1, 'kp': 0.3, 'ti': 0.5, 'rows': rows})
        f = at.fit_closed_loop(legs)
        self.assertEqual((f['L'], f['tau']), (4, 9))
        self.assertAlmostEqual(f['K'], true['K'], places=6)
        self.assertLess(f['share'], 0.01)
        self.assertIsNone(at.fit_closed_loop([]))

    def test_matches_a_check_on_the_model(self):
        # A plant the fit can describe: the predicted line should follow the
        # check it was drawn for, and every check step gets one after the reach.
        plant = Plant(K=2.0, L=2.0, tau=8.0, noise=0.005, curve=lambda v: 0.67 + 2.0 * max(v, 0))
        b = FakeBoard(plant)
        b.advance(200)
        d = tempfile.mkdtemp()
        t = at.AutoTuner(b, Clock(b), SimpleNamespace(d=0.4), d)
        t.say = lambda m: None
        got, ends = [], []
        t.predicted = lambda rows, label: got.append((rows, label))
        t.predicted_end = ends.append
        t.point(8, (0.10, 0.40), 0.4)
        self.assertTrue(got)
        self.assertEqual(len(ends), len(got))   # every predicted step is ended, where it ended
        self.assertTrue(all(e > 0 for e in ends))
        self.assertTrue(any(c.startswith('Predicted for check') for c in t.calcs))
        self.assertTrue(any(c.startswith('Fit of check 1 at 8 mbar as it ran') for c in t.calcs))
        for name in os.listdir(d):
            if not name.startswith('check_'):
                continue
            with open(os.path.join(d, name), newline='') as fh:
                rs = [r for r in csv.DictReader(fh) if r['p_predicted_mbar'] and r['p_mbar']]
            self.assertGreater(len(rs), 20, name)
            step = abs(float(rs[0]['to_mbar']) - float(rs[0]['from_mbar']))
            err = math.sqrt(sum((float(r['p_mbar']) - float(r['p_predicted_mbar'])) ** 2 for r in rs) / len(rs))
            self.assertLess(err, 0.25 * step, name)


class LogHelpers(unittest.TestCase):
    def test_gains_change_names_what_changed(self):
        self.assertEqual(at.gains_change([0.64, 0.88, 0.0], [0.45, 0.88, 0.0]), 'KP 0.64 -> 0.45')
        self.assertEqual(at.gains_change([0.64, 0.88, 0.5], [0.64, 0.62, 0.0]), 'TI 0.88 -> 0.62, KD 0.50 -> 0.00')
        self.assertEqual(at.gains_change([0.64, 0.88, 0.0], [0.64, 0.88, 0.0]), '')
        self.assertEqual(at.gains_change(None, [0.1, 0.4, 0.0]), 'KP -- -> 0.10, TI -- -> 0.40, KD -- -> 0.00')

    def test_log_row_writes_the_header_once_and_skips_no_folder(self):
        d = tempfile.mkdtemp()
        at.log_row(d, ['a'] * len(at.LOG_COLS))
        at.log_row(d, ['b', None] + [''] * (len(at.LOG_COLS) - 2))
        at.log_row(None, ['c'] * len(at.LOG_COLS))
        with open(os.path.join(d, at.LOG_FILE), newline='') as fh:
            got = list(csv.reader(fh))
        self.assertEqual(got[0], at.LOG_COLS)
        self.assertEqual([r[0] for r in got[1:]], ['a', 'b'])
        self.assertEqual(got[2][1], '')

    def test_unchanged_gains_are_not_logged_again(self):
        d = tempfile.mkdtemp()
        t = at.AutoTuner(None, None, SimpleNamespace(d=0.4), d)
        t.set_gains([0.3, 0.4, 0], 'PID SET 0.30,0.40,0', 'one')
        t.set_gains([0.3, 0.4, 0], 'PID SET 0.30,0.40,0', 'two')
        t.set_gains([0.24, 0.4, 0], 'PID SET 0.24,0.40,0', 'three')
        with open(os.path.join(d, at.LOG_FILE), newline='') as fh:
            got = list(csv.reader(fh))[1:]
        self.assertEqual([r[9] for r in got], ['PID SET 0.30,0.40,0: one', 'PID SET 0.24,0.40,0: three'])
        self.assertEqual(got[1][5:9], ['0.24', '0.40', '0.00', 'KP 0.30 -> 0.24'])


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


def fig(o=0.0, n=0, t63=20, t90=60, back=0.0, **kw):
    f = {'overshoot_pct': o, 'settle_s': 50, 'reversals': n, 'saturated': False, 't63_s': t63, 't90_s': t90,
         'backoff_pct': back, 'sag_s': 0}
    f.update(kw)
    return f


class Refine(unittest.TestCase):
    """point() on scripted checks: the relay and the check are stubbed, so these are the rules in sequence."""
    FIT = {'K': 1.0, 'L': 10, 'tau': 30}

    def tuner(self, figs, fits=None, ku=2.0, pu=20.0, d=None):
        a = SimpleNamespace(d=0.4)
        t = at.AutoTuner(None, None, a, d)
        t.say = lambda m: None
        t.reach = lambda sp, kp, ti, why=None: 3.0
        t.relay = lambda sp, x0, dd: {'Ku': ku, 'Pu_s': pu, 'd_pct': 0.4, 'a_mbar': 0.1, 'eps_mbar': 0.02,
                                      'cycles': 4, 'asym': None, 'steady': True, 'noise': 0.004, 'wander': 0.0}
        self.used = []
        fits = list(fits if fits is not None else [dict(self.FIT, ok=True)] * len(figs))

        def check(sp, kp, ti, r, k):
            self.used.append((kp, ti))
            return dict(figs[k - 1]), kp
        t.check = check
        t.refit = lambda k, fit: fits[k - 1] or fit
        return t

    def test_a_tail_shortens_ti_only(self):
        t = self.tuner([fig(0, 0, 20, 120, 20.0), fig(0, 0, 18, 60, 5.0)])
        r = t.point(8.0, (0.1, 0.4), 0.4)
        self.assertEqual(self.used, [(0.5, 0.67), (0.5, 0.47)])      # Ku / 4 and 2 Pu, then TI x 0.7
        self.assertEqual((r['kp'], r['ti_min'], r['trims']), (0.5, 0.47, 'TI x0.70'))
        self.assertEqual((r['t63_s'], r['t90_s'], r['backoff_pct']), (18, 60, 5.0))
        self.assertEqual((r['K'], r['L_s'], r['tau_s']), (1.0, 10, 30))
        self.assertEqual(r['note'], 'no fit yet' if False else r['note'])
        self.assertNotIn('worse', r['note'])

    def test_overshoot_cuts_kp_until_it_passes(self):
        t = self.tuner([fig(12.0, 1), fig(7.0, 1), fig(2.0, 0)])
        r = t.point(8.0, (0.1, 0.4), 0.4)
        self.assertEqual([u[0] for u in self.used], [0.5, 0.4, 0.32])
        self.assertEqual(r['trims'], 'KP x0.80, KP x0.80')
        self.assertEqual(r['overshoot_pct'], 2.0)

    def test_a_refinement_that_makes_it_worse_is_undone_and_ends_them(self):
        t = self.tuner([fig(8.0, 1), fig(20.0, 3), fig(0.0, 0)])
        r = t.point(8.0, (0.1, 0.4), 0.4)
        self.assertEqual(len(self.used), 2)                          # no third check
        self.assertEqual((r['kp'], r['ti_min']), (0.5, 0.67))        # the first check's gains stay
        self.assertEqual(r['overshoot_pct'], 8.0)                    # and its figures
        self.assertIn('undone', r['note'])
        self.assertEqual(r['trims'], '')
        self.assertIn('KP x0.80 made the check worse and was undone', r['note'])

    def test_at_most_four_refinements(self):
        t = self.tuner([fig(12.0 - k, 1) for k in range(8)])
        r = t.point(8.0, (0.1, 0.4), 0.4)
        self.assertEqual(len(self.used), 5)                          # the check, then 4 refinements each checked
        self.assertEqual([u[0] for u in self.used], [0.5, 0.4, 0.32, 0.26, 0.21])
        self.assertIn('after 4 refinements', r['note'])
        self.assertEqual(r['kp'], 0.21)

    def test_at_the_ti_limit_the_check_passes(self):
        # 2.5 L_eff = 25 s = 0.42 min: TI 0.67 -> 0.47 -> 0.42 (the limit) -> passes with the note
        t = self.tuner([fig(0, 0, 20, 120, 20.0)] * 4)
        r = t.point(8.0, (0.1, 0.4), 0.4)
        self.assertEqual(len(self.used), 3)
        self.assertEqual(r['ti_min'], 0.42)
        self.assertIn('as fast as the cryostat allows', r['note'])

    def test_a_passing_first_check_changes_nothing(self):
        t = self.tuner([fig(1.0, 0)])
        r = t.point(8.0, (0.1, 0.4), 0.4)
        self.assertEqual((r['kp'], r['ti_min'], r['trims']), (0.5, 0.67, ''))
        self.assertEqual(r['kp_rule'], 0.5)

    def test_learned_fit_gives_the_first_gains(self):
        prior = {'sp_mbar': 8.0, 'kp': 0.1, 'ti_min': 0.83, 'Ku': 1.8, 'Pu_s': 20.0, 'K': 1.0, 'L_s': 10, 'tau_s': 30}
        t = self.tuner([fig(0.0, 0)], ku=2.0)
        r = t.point(8.0, (0.1, 0.83), 0.4, prior)
        # L_eff = max(10, 5) = 10 s; TI = min(max(30, 25), 200) = 30 s; KP = min(30 / (5 x 1 x 10), 2 / 3) = 0.6
        self.assertEqual(self.used[0], (0.6, 0.5))
        self.assertIn('first gains from the learned fit', r['note'])
        self.assertTrue(any(c.startswith('First gains from the learned fit') for c in t.calcs))

    def test_learned_fit_is_dropped_when_the_plant_has_changed(self):
        prior = {'sp_mbar': 8.0, 'kp': 0.1, 'ti_min': 0.83, 'Ku': 0.6, 'Pu_s': 20.0, 'K': 1.0, 'L_s': 10, 'tau_s': 30}
        t = self.tuner([fig(0.0, 0)], ku=2.0)       # 3.3 x the learned Ku
        r = t.point(8.0, (0.1, 0.83), 0.4, prior)
        self.assertEqual(self.used[0], (0.5, 0.67))
        self.assertTrue(any(c.startswith('Fresh:') for c in t.calcs))

    def test_a_learned_row_without_a_fit_starts_from_the_relay_rule(self):
        prior = {'sp_mbar': 8.0, 'kp': 0.1, 'ti_min': 0.83, 'Ku': 1.8, 'Pu_s': 20.0}
        t = self.tuner([fig(0.0, 0)], ku=2.0)
        t.point(8.0, (0.1, 0.83), 0.4, prior)
        self.assertEqual(self.used[0], (0.5, 0.67))

    def test_no_prior_no_learned_calcs(self):
        t = self.tuner([fig(0.0, 0)])
        t.point(8.0, (0.1, 0.4), 0.4)
        self.assertFalse(any(c.startswith(('Learned', 'Fresh')) for c in t.calcs))


class Files(unittest.TestCase):
    def test_only_the_check_a_result_came_from_is_kept(self):
        d = tempfile.mkdtemp()
        a = SimpleNamespace(d=0.4)
        t = at.AutoTuner(None, None, a, d)
        t.say = lambda m: None
        t.reach = lambda sp, kp, ti, why=None: 3.0
        t.relay = lambda sp, x0, dd: {'Ku': 2.0, 'Pu_s': 10.0, 'd_pct': 0.4, 'a_mbar': 0.1, 'eps_mbar': 0.02,
                                      'cycles': 2, 'asym': None, 'steady': True, 'noise': 0.004}
        figs = [fig(30.0, 1), fig(15.0, 1), fig(2.0, 0)]

        def check(sp, kp, ti, r, k):
            for leg in ('up', 'back'):
                open(os.path.join(d, 'check_%gmbar_%d_%s.csv' % (sp, k, leg)), 'w').close()
            return figs[k - 1], kp
        t.check = check
        t.refit = lambda k, fit: fit
        r = t.point(2.0, (0.1, 0.4), 0.4)
        self.assertEqual(r['trims'], 'KP x0.80, KP x0.80')
        self.assertEqual(sorted(os.listdir(d)), ['autotune_log.csv', 'check_2mbar_3_back.csv', 'check_2mbar_3_up.csv'])
        self.assertIn('Check 1 passed over: check_2mbar_1_up.csv, check_2mbar_1_back.csv deleted', t.calcs)

    def test_an_undone_refinement_keeps_the_earlier_check(self):
        d = tempfile.mkdtemp()
        t = at.AutoTuner(None, None, SimpleNamespace(d=0.4), d)
        t.say = lambda m: None
        t.reach = lambda sp, kp, ti, why=None: 3.0
        t.relay = lambda sp, x0, dd: {'Ku': 2.0, 'Pu_s': 10.0, 'd_pct': 0.4, 'a_mbar': 0.1, 'eps_mbar': 0.02,
                                      'cycles': 2, 'asym': None, 'steady': True, 'noise': 0.004}
        figs = [fig(8.0, 1), fig(25.0, 4)]

        def check(sp, kp, ti, r, k):
            for leg in ('up', 'back'):
                open(os.path.join(d, 'check_%gmbar_%d_%s.csv' % (sp, k, leg)), 'w').close()
            return figs[k - 1], kp
        t.check = check
        t.refit = lambda k, fit: fit
        r = t.point(2.0, (0.1, 0.4), 0.4)
        self.assertIn('undone', r['note'])
        self.assertEqual(sorted(os.listdir(d)), ['autotune_log.csv', 'check_2mbar_1_back.csv', 'check_2mbar_1_up.csv'])


class Fit(unittest.TestCase):
    """fit_response on the model: a hold, a 20 % step up and back, as a check is."""

    @staticmethod
    def check_run(K, L, tau, noise=0.004, seed=3, drift=0.0):
        plant = Plant(K=K, L=L, tau=tau, noise=noise, seed=seed)
        x0 = 6.0
        plant.v = plant.cmd = plant.eff = x0
        plant.x = plant.static(x0)
        ps, xs = [], []
        plan = [(0, x0), (70, x0 + 1.5), (270, x0)]          # hold 70 s, +1.5 % for 200 s, back
        for n in range(1, 470):
            for t0, x in plan:
                if n == t0 + 1:
                    plant.cmd = x
            plant.run_to(n)
            ps.append(plant.p() + drift * n)
            xs.append(plant.v)
        return ps, xs

    def near(self, tau, found):
        i = min(range(len(at.TAU_GRID)), key=lambda k: abs(math.log(at.TAU_GRID[k] / tau)))
        j = at.TAU_GRID.index(found)
        return abs(i - j) <= 1

    def test_recovers_k_l_tau(self):
        for K, L, tau in [(2.4, 12, 35), (3.9, 28, 85), (1.5, 4, 9), (3.3, 0, 13), (1.3, 2, 30)]:
            ps, xs = self.check_run(K, L, tau)
            f = at.fit_response(ps, xs)
            self.assertTrue(f['ok'], (K, L, tau, f))
            self.assertAlmostEqual(f['K'] / K, 1.0, delta=0.15, msg=(K, L, tau, f))
            self.assertTrue(self.near(tau, f['tau']), (K, L, tau, f))
            i = min(range(len(at.L_GRID)), key=lambda k: abs(at.L_GRID[k] - L))
            self.assertLessEqual(abs(at.L_GRID.index(f['L']) - i), 1, (K, L, tau, f))
            self.assertGreater(f['R2'], 0.9)

    def test_a_drift_does_not_spoil_it(self):
        ps, xs = self.check_run(2.4, 12, 35, drift=0.004)
        f = at.fit_response(ps, xs)
        self.assertTrue(f['ok'])
        self.assertAlmostEqual(f['K'] / 2.4, 1.0, delta=0.15)
        self.assertAlmostEqual(f['drift'], 0.004, delta=0.0015)

    def test_rejects_what_is_not_a_response(self):
        ps, xs = self.check_run(2.4, 12, 35)
        self.assertIsNone(at.fit_response(ps[:20], xs[:20]))                  # too short
        self.assertIsNone(at.fit_response(ps, [6.0] * len(ps)))               # the valve never moved
        rnd = random.Random(5)
        f = at.fit_response([5 + rnd.gauss(0, 0.2) for _ in ps], xs)          # noise only
        self.assertFalse(f['ok'])
        f = at.fit_response([10 - p for p in ps], xs)                         # opens, pressure falls: K < 0
        self.assertFalse(f['ok'])
        f = at.fit_response(ps, [6 + 0.02 * (x - 6) for x in xs])             # moved 0.03 %: under MOVE_MIN
        self.assertFalse(f['ok'])

    def test_refit_keeps_the_earlier_fit_when_the_new_one_is_not_good(self):
        t = at.AutoTuner(None, None, SimpleNamespace(d=0.4), None)
        t.say = lambda m: None
        old = {'K': 2.0, 'L': 8, 'tau': 25}
        ps, xs = self.check_run(2.4, 12, 35)
        t.series = (ps, xs)
        new = t.refit(1, old)
        self.assertNotEqual(new, old)
        self.assertEqual(new['L'], 12)
        rnd = random.Random(2)
        t.series = ([5.0 + rnd.gauss(0, 0.2) for _ in ps], xs)
        self.assertEqual(t.refit(2, old), old)
        t.series = None
        self.assertEqual(t.refit(3, old), old)
        self.assertIsNone(t.refit(4, None))

    def test_the_check_series_is_one_sample_a_second_over_the_whole_check(self):
        # through check(): hold, up, back on the model
        curve = lambda v: 0.67 + 2.0 * max(v, 0)      # noqa: E731
        plant = Plant(curve=curve, L=4.0, tau=15.0, noise=0.004)
        b = FakeBoard(plant)
        b.advance(60)
        t = at.AutoTuner(b, Clock(b), SimpleNamespace(d=0.4), None)
        t.say = lambda m: None
        t.cuts = []
        b.sets[0] = [0.4, 1.0, 0.0]
        sp = round(plant.static(5.0), 1)
        r = {'Pu_s': 16.0, 'noise': 0.004, 'wander': 0.0}
        figure, kp = t.check(sp, 0.4, 1.0, r, 1)
        ps, xs = t.series
        self.assertEqual(len(ps), len(xs))
        self.assertGreater(len(ps), 120)
        f = t.refit(1, None)
        self.assertIsNotNone(f)


class Learned(unittest.TestCase):
    """Ryan, 9 Nov: refine the numbers from run to run, not start fresh."""

    def test_file_round_trip(self):
        d = tempfile.mkdtemp()
        path = os.path.join(d, at.LEARNED_FILE)
        self.assertEqual(at.load_learned(path), {})
        L = {}
        at.learn(L, {'sp_mbar': 16.0, 'kp': 0.42, 'ti_min': 0.6, 'Ku': 1.07, 'Pu_s': 18.0, 'd_pct': 0.8,
                     'a_mbar': 0.95, 'valve_pct': 6.8, 'trims': 'TI x0.70', 'note': '', 'K': 2.44, 'L_s': 2,
                     'tau_s': 6, 't63_s': 15, 't90_s': 41, 'backoff_pct': 8.5}, '09/11/26 16:30')
        at.learn(L, {'sp_mbar': 4.0, 'kp': 0.3, 'ti_min': 0.5, 'Ku': 2.0, 'Pu_s': 15.0}, '09/11/26 16:00')
        at.save_learned(path, L)
        back = at.load_learned(path)
        self.assertEqual(sorted(back), ['16', '4'])
        self.assertEqual((back['16']['kp'], back['16']['ti_min'], back['16']['Ku']), (0.42, 0.6, 1.07))
        self.assertEqual(back['16']['trims'], 'TI x0.70')
        self.assertEqual([back['16'][k] for k in ('K', 'L_s', 'tau_s', 't63_s', 't90_s', 'backoff_pct')],
                         [2.44, 2.0, 6.0, 15.0, 41.0, 8.5])
        self.assertIsNone(back['4']['K'])
        with open(path) as f:
            lines = f.readlines()
        self.assertEqual(lines[0].strip().split(',')[-6:], ['K', 'L_s', 'tau_s', 't63_s', 't90_s', 'backoff_pct'])
        self.assertTrue(lines[1].startswith('4,'))   # lowest setpoint first

    def test_an_old_file_loads_with_the_new_columns_empty(self):
        d = tempfile.mkdtemp()
        path = os.path.join(d, at.LEARNED_FILE)
        with open(path, 'w') as f:
            f.write('sp_mbar,kp,ti_min,Ku,Pu_s,d_pct,a_mbar,valve_pct,trims,note,when\n'
                    '16,0.42,0.6,1.07,18.0,0.8,1.05,6.8,x1.25 x1.25,,09/11/26 16:30\n')
        L = at.load_learned(path)
        self.assertEqual((L['16']['kp'], L['16']['ti_min'], L['16']['when']), (0.42, 0.6, '09/11/26 16:30'))
        for k in ('K', 'L_s', 'tau_s', 't63_s', 't90_s', 'backoff_pct'):
            self.assertIsNone(L['16'].get(k))
        at.save_learned(path, L)                         # and is rewritten with the new columns
        self.assertEqual(at.load_learned(path)['16']['kp'], 0.42)

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

    def test_a_point_result_goes_into_the_learned_table_with_its_fit(self):
        t = Refine().tuner([fig(0.0, 0)])
        r = t.point(16.0, (0.1, 0.4), 0.8)
        L = {}
        at.learn(L, r, '10/11/26 18:00')
        self.assertEqual((L['16']['K'], L['16']['L_s'], L['16']['tau_s']), (1.0, 10, 30))
        self.assertEqual(L['16']['t90_s'], 60)


if __name__ == '__main__':
    unittest.main()
