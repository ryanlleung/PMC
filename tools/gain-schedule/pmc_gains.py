#!/usr/bin/env python3
"""Gain schedule for the AUTO loop, run from a PC over the text port.

One KP and TI does not suit every setpoint: the pressure moves several times
more per % of valve at 1 mbar than at 12 mbar (the pump line passes less gas
at low pressure, and the pot holds a liquid pool at some setpoints and only
gas at others). This program measures a KP and TI for a few setpoints and
then keeps the board's KP and TI matched to its setpoint.

    python pmc_gains.py tune 192.168.1.100 --sp 2 5 10       # step tests -> gains.csv
    python pmc_gains.py show gains.csv                        # the blended values, 1 to 25 mbar
    python pmc_gains.py run 192.168.1.100 gains.csv           # keep KP and TI matched to the setpoint
    python pmc_gains.py fit steps/sp5.0_up.csv                # refit one recorded step

tune: for each setpoint it runs AUTO until the pressure holds, switches to
MANUAL at the valve position AUTO settled at, steps the valve up by --step %
and back down, and fits a gain, a delay and a time constant to each response.
KP and TI follow from the SIMC rules (Skogestad): KP = tau / (K (tc + delay)),
TI = min(tau, 4 (tc + delay)), tc = delay x --tc. A step that moves the
pressure too little is doubled and one that moves it too far is halved, both
automatically. At the end the setpoint and mode it found are put back. Run it
with the heater at its usual power: the gains change with it.

run: polls PRES SP?;MODE?;PID? every --every s. When the setpoint changes, or
at connect, it writes the KP and TI blended from the table for that setpoint
(log-log between rows, the end rows outside them) with PID SET, which does
what the Home PID tile does. A KP or TI typed on the screen stays until the
next setpoint change.

The table is a CSV with at least the columns sp_mbar, kp and ti_min, one row
per setpoint, and can be edited by hand. KP is in % per mbar and TI in
minutes, as on the PID tile.

Needs Python 3.8 or later and nothing else (no pip). Text port 8888 (give
host:port for another). The board takes one connection at a time and a new
one replaces the old: do not run this beside the logger or the Frappy driver
on the same board (the Frappy driver can import gains_for() instead). The
PID commands are in firmware 0.15.8 on; tune works on any 0.13.0 or later.
"""
import argparse
import csv
import math
import os
import socket
import sys
import time

TCP_PORT = 8888

# Firmware limits for KP and TI (cfg.c tune_lo/tune_hi, two decimals).
KP_MIN, KP_MAX = 0.01, 100.0
TI_MIN, TI_MAX = 0.01, 100.0


# ---------------------------------------------------------------------------
# The table: rows of (setpoint, KP, TI), blended log-log on the setpoint.
# ---------------------------------------------------------------------------

def load_table(path):
    """Rows (sp_mbar, kp, ti_min) from a CSV, sorted by setpoint."""
    rows = []
    with open(path, newline='') as f:
        for r in csv.DictReader(f):
            try:
                sp, kp, ti = float(r['sp_mbar']), float(r['kp']), float(r['ti_min'])
            except (KeyError, TypeError, ValueError):
                continue
            if sp > 0 and kp > 0 and ti > 0:
                rows.append((sp, kp, ti))
    rows.sort()
    if not rows:
        raise ValueError(f'{path}: no rows with sp_mbar, kp and ti_min above 0')
    return rows


def _loglog(x, xs, ys):
    if x <= xs[0]:
        return ys[0]
    if x >= xs[-1]:
        return ys[-1]
    for i in range(1, len(xs)):
        if x <= xs[i]:
            f = (math.log(x) - math.log(xs[i - 1])) / (math.log(xs[i]) - math.log(xs[i - 1]))
            return math.exp(math.log(ys[i - 1]) + f * (math.log(ys[i]) - math.log(ys[i - 1])))
    return ys[-1]


def gains_for(table, sp):
    """KP (% per mbar) and TI (min) for a setpoint, rounded as the board keeps them."""
    xs = [r[0] for r in table]
    kp = _loglog(max(sp, 1e-3), xs, [r[1] for r in table])
    ti = _loglog(max(sp, 1e-3), xs, [r[2] for r in table])
    kp = min(max(round(kp, 2), KP_MIN), KP_MAX)
    ti = min(max(round(ti, 2), TI_MIN), TI_MAX)
    return kp, ti


def simc(K, delay, tau, tc=1.0):
    """KP (% per mbar) and TI (min) from gain K (mbar per %), delay and tau (s)."""
    c = tc * delay
    kp = tau / (K * (c + delay))
    ti = min(tau, 4 * (c + delay)) / 60.0
    return min(max(kp, KP_MIN), KP_MAX), min(max(ti, TI_MIN), TI_MAX)


# ---------------------------------------------------------------------------
# Fitting a step: first order plus delay, least squares over a grid.
# ---------------------------------------------------------------------------

def fit_step(t, dp, dx):
    """Fit dp(t) = K dx (1 - exp(-(t - delay) / tau)) for t > delay.

    t: seconds from the valve command; dp: pressure minus the baseline (mbar);
    dx: the valve step (%). Returns dict K (mbar per %), delay, tau (s), the
    final change dp_end and the fit's rms error, or None with too few points.
    """
    if len(t) < 10:
        return None
    if len(t) > 400:   # every other point is plenty for the grid
        k = len(t) // 300
        t, dp = t[::k], dp[::k]
    span = t[-1]
    sy2 = sum(y * y for y in dp)
    best = None
    taus = [2.0 * (1.12 ** i) for i in range(70)]       # 2 s to about 5000 s
    def scan(delays):
        nonlocal best
        for d in delays:
            for tau in taus:
                sys_ = syy = 0.0
                for ti, yi in zip(t, dp):
                    if ti > d:
                        s = 1.0 - math.exp(-(ti - d) / tau)
                        sys_ += yi * s
                        syy += s * s
                if syy <= 0:
                    continue
                sse = sy2 - sys_ * sys_ / syy
                if best is None or sse < best[0]:
                    best = (sse, d, tau, sys_ / syy)
    scan([float(d) for d in range(0, int(max(2, min(180, span / 2))), 2)])
    d0 = best[1]
    scan([d0 + 0.5 * i for i in range(-4, 5) if d0 + 0.5 * i >= 0])
    sse, delay, tau, amp = best
    return dict(K=amp / dx, delay=delay, tau=tau, dp_end=amp,
                rms=math.sqrt(max(sse, 0.0) / len(t)))


# ---------------------------------------------------------------------------
# The text port.
# ---------------------------------------------------------------------------

class Refused(Exception):
    """The board answered ERR."""


class Link:
    def __init__(self, host, timeout=10.0):
        h, _, p = host.partition(':')
        self.addr = (h, int(p) if p else TCP_PORT)
        self.timeout = timeout
        self.sock = None
        self.buf = b''

    def connect(self):
        self.close()
        self.sock = socket.create_connection(self.addr, timeout=self.timeout)
        self.buf = b''

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
        self.sock = None

    def ask(self, line):
        """Send one line, return the reply's parts; raise Refused on ERR."""
        if not self.sock:
            self.connect()
        self.sock.sendall(line.encode('ascii') + b'\r\n')
        while b'\n' not in self.buf:
            d = self.sock.recv(4096)
            if not d:
                raise ConnectionError('board closed the connection')
            self.buf += d
        reply, _, self.buf = self.buf.partition(b'\n')
        parts = reply.decode('latin1').strip().split(';')
        if parts and parts[0].startswith('ERR'):
            raise Refused(parts[0])
        return parts

    def do(self, cmd):
        """A set command (silent when accepted), followed by MODE? so there is a reply."""
        return self.ask(cmd + ';MODE?')


def value(part, head):
    """'PRES 2.913 mbar' -> 2.913; 'PRES NONE' -> None."""
    if not part.startswith(head):
        raise ValueError(f'expected {head}, got {part!r}')
    v = part[len(head):].split()
    if not v or v[0] == 'NONE':
        return None
    return float(v[0])


# ---------------------------------------------------------------------------
# tune
# ---------------------------------------------------------------------------

class Clock:
    """Board seconds; --speed > 1 only for a simulator run faster than real time."""
    def __init__(self, speed=1.0):
        self.speed = speed
        self.t0 = time.monotonic()

    def now(self):
        return (time.monotonic() - self.t0) * self.speed

    def wait_until(self, t):
        dt = (t - self.now()) / self.speed
        if dt > 0:
            time.sleep(dt)


class Tuner:
    def __init__(self, link, clock, a, log_dir):
        self.link, self.clock, self.a, self.log_dir = link, clock, a, log_dir

    def say(self, msg):
        print(f'[{self.clock.now():7.0f} s] {msg}', flush=True)

    def sample(self):
        p, x, m = self.link.ask('PRES?;VALV?;MODE?')
        return value(p, 'PRES'), value(x, 'VALV'), m.partition(' ')[2]

    def samples(self, seconds, stop=None):
        """Read once a board second for up to `seconds`; stop(rows) ends early."""
        rows, t0 = [], self.clock.now()
        nxt = t0
        while True:
            self.clock.wait_until(nxt)
            nxt += 1.0
            p, x, m = self.sample()
            if p is not None:
                rows.append((self.clock.now() - t0, p, x, m))
            if rows and rows[-1][0] >= seconds:
                return rows
            if stop and stop(rows):
                return rows

    def hold_auto(self, sp):
        """AUTO at sp until the pressure holds; returns the valve % it held at."""
        self.link.do(f'PRES SP {sp:.1f}')
        if self.sample()[2] != 'AUTO':
            self.link.do('MODE AUTO')
        tol = max(0.05, 0.03 * sp)
        win = self.a.hold

        def held(rows):
            last = [r for r in rows if r[0] >= rows[-1][0] - win]
            return (rows[-1][0] >= win and all(abs(r[1] - sp) < tol for r in last))
        self.say(f'AUTO at {sp:g} mbar, waiting for the pressure to hold within {tol:.2f} mbar for {win:.0f} s')
        rows = self.samples(self.a.settle_max, held)
        last = [r for r in rows if r[0] >= rows[-1][0] - win and r[2] is not None]
        x0 = round(sum(r[2] for r in last) / len(last), 1)
        if held(rows):
            self.say(f'held, valve {x0:.1f} %')
        else:
            self.say(f'did not hold in {self.a.settle_max:.0f} s (the gains in use may be too high here); '
                     f'using the mean valve position {x0:.1f} %')
        return x0

    def steady(self, sp, seconds_min=60, seconds_max=600):
        """MANUAL: wait until the 30 s mean stops moving; returns baseline and noise."""
        tol = max(0.01, 0.005 * sp)

        def calm(rows):
            if rows[-1][0] < seconds_min:
                return False
            a = [r[1] for r in rows if r[0] >= rows[-1][0] - 30]
            b = [r[1] for r in rows if rows[-1][0] - 60 <= r[0] < rows[-1][0] - 30]
            return bool(a and b) and abs(sum(a) / len(a) - sum(b) / len(b)) < tol
        rows = self.samples(seconds_max, calm)
        base = [r[1] for r in rows if r[0] >= rows[-1][0] - 30]
        m = sum(base) / len(base)
        sd = math.sqrt(sum((v - m) ** 2 for v in base) / len(base))
        return m, sd

    def one_step(self, sp, x_from, x_to, p0, max_dp, name):
        """Move the valve and record until the fit says it has settled (or it runs away)."""
        self.link.do(f'VALV SET {x_to:.1f}')
        t0 = self.clock.now()
        rows, fit, nxt_fit, away, n = [], None, 60.0, False, 0
        while True:
            n += 1
            self.clock.wait_until(t0 + n)
            p, x, _ = self.sample()
            t = self.clock.now() - t0
            if p is None:
                continue
            rows.append((t, p, x))
            if abs(p - p0) > max_dp:
                away = True
                break
            if t >= nxt_fit:
                nxt_fit = t + 30
                fit = fit_step([r[0] for r in rows], [r[1] - p0 for r in rows], x_to - x_from)
                if fit and t >= max(90.0, fit['delay'] + 3 * fit['tau']):
                    break
            if t >= self.a.step_max:
                break
        if self.log_dir:
            with open(os.path.join(self.log_dir, f'{name}.csv'), 'w', newline='') as f:
                w = csv.writer(f)
                w.writerow(['t_s', 'p_mbar', 'valve_pct', 'p0_mbar', 'valve_from', 'valve_to'])
                for r in rows:
                    w.writerow([f'{r[0]:.1f}', f'{r[1]:.3f}', '' if r[2] is None else f'{r[2]:.2f}',
                                f'{p0:.3f}', f'{x_from:.1f}', f'{x_to:.1f}'])
        if away:
            return None, True
        fit = fit_step([r[0] for r in rows], [r[1] - p0 for r in rows], x_to - x_from)
        return fit, False

    def tune_point(self, sp, step):
        x0 = self.hold_auto(sp)
        self.x0 = x0
        self.link.do('MODE MANUAL')
        self.link.do(f'VALV SET {x0:.1f}')
        max_dp = max(0.3, self.a.max_dp * sp)
        for attempt in range(4):
            p0, sd = self.steady(sp)
            small = max(self.a.min_dp * sp, 5 * sd, 0.02)
            x1 = min(100.0, x0 + step)
            self.say(f'MANUAL at {x0:.1f} %, baseline {p0:.3f} mbar (noise {sd:.3f}); step {x0:.1f} -> {x1:.1f} %')
            up, away = self.one_step(sp, x0, x1, p0, max_dp, f'sp{sp:g}_up')
            if away:
                self.link.do(f'VALV SET {x0:.1f}')
                step = round(step / 2, 1)
                self.say(f'pressure moved more than {max_dp:.2f} mbar: back to {x0:.1f} %, step halved to {step:.1f} %')
                if step < 0.1:
                    raise RuntimeError('step below 0.1 % still moves the pressure too far')
                continue
            if up is None or abs(up['dp_end']) < small:
                self.link.do(f'VALV SET {x0:.1f}')
                if step * 2 > self.a.step_limit:
                    raise RuntimeError(f'a {step:.1f} % step moved the pressure less than {small:.3f} mbar')
                step = round(step * 2, 1)
                self.say(f'pressure moved too little: back to {x0:.1f} %, step doubled to {step:.1f} %')
                continue
            self.say(f'  up:   K {up["K"]:.3f} mbar/%  delay {up["delay"]:.1f} s  tau {up["tau"]:.0f} s')
            p1, _ = self.steady(sp, 30, 300)
            dn, away = self.one_step(sp, x1, x0, p1, max_dp, f'sp{sp:g}_down')
            fits = [up]
            if dn and not away:
                self.say(f'  down: K {dn["K"]:.3f} mbar/%  delay {dn["delay"]:.1f} s  tau {dn["tau"]:.0f} s')
                fits.append(dn)
            K = sum(f['K'] for f in fits) / len(fits)
            delay = max(max(f['delay'] for f in fits), 1.0)   # the loop runs once a second
            tau = max(sum(f['tau'] for f in fits) / len(fits), 1.0)
            kp, ti = simc(K, delay, tau, self.a.tc)
            self.say(f'{sp:g} mbar: KP {kp:.2f} %/mbar, TI {ti:.2f} min')
            return dict(sp_mbar=sp, kp=round(kp, 2), ti_min=round(ti, 2), K_mbar_per_pct=round(K, 4),
                        delay_s=round(delay, 1), tau_s=round(tau, 1), step_pct=step, valve_pct=x0,
                        p0_mbar=round(p0, 3))
        raise RuntimeError('no usable step after 4 tries')


def cmd_tune(a):
    link = Link(a.host)
    clock = Clock(a.speed)
    log_dir = a.log or os.path.splitext(a.out)[0] + '_steps'
    os.makedirs(log_dir, exist_ok=True)
    t = Tuner(link, clock, a, log_dir)
    t.x0 = None
    sp0, mode0 = None, None
    try:
        sp0 = value(link.ask('PRES SP?')[0], 'PRES SP')
        mode0 = t.sample()[2]
    except Refused as e:
        sys.exit(f'board refused a query: {e}')
    t.say(f'board setpoint {sp0} mbar, mode {mode0}; steps logged in {log_dir}')
    rows, step = [], a.step
    try:
        for sp in a.sp:
            rows.append(t.tune_point(sp, step))
            write_table(a.out, rows)
            # Next first step from this gain: aim at a change of 10 % of the next setpoint.
            K = rows[-1]['K_mbar_per_pct']
            nxt = a.sp[min(len(rows), len(a.sp) - 1)]
            step = min(max(round(0.10 * nxt / K, 1), 0.1), a.step_limit)
    except KeyboardInterrupt:
        t.say('stopped by Ctrl+C')
    except (Refused, RuntimeError) as e:
        t.say(f'stopped: {e}')
    finally:
        # The valve back where AUTO held it at the last setpoint tested, then
        # the setpoint and mode found at the start. Each is tried on its own.
        back = []
        if t.x0 is not None:
            back.append(f'VALV SET {t.x0:.1f}')
        if sp0 is not None:
            back.append(f'PRES SP {sp0:.1f}')
        if mode0 == 'AUTO':
            back.append('MODE AUTO')
        for c in back:
            try:
                link.do(c)
                t.say(f'put back: {c}')
            except (OSError, ConnectionError, Refused) as e:
                t.say(f'could not put back ({c}): {e}')
        link.close()
    if rows:
        print(f'\nwrote {a.out}:')
        show(load_table(a.out), rows)


def write_table(path, rows):
    cols = ['sp_mbar', 'kp', 'ti_min', 'K_mbar_per_pct', 'delay_s', 'tau_s', 'step_pct', 'valve_pct', 'p0_mbar']
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow(r)


# ---------------------------------------------------------------------------
# show, fit
# ---------------------------------------------------------------------------

def show(table, rows=None, sps=None):
    if rows:
        print(' setpoint   K mbar/%   delay s   tau s    KP %/mbar   TI min')
        for r in rows:
            print(f'{r["sp_mbar"]:7.1f}   {r["K_mbar_per_pct"]:8.3f}   {r["delay_s"]:7.1f}  {r["tau_s"]:6.0f}'
                  f'   {r["kp"]:9.2f}   {r["ti_min"]:6.2f}')
        print()
    print(' setpoint   KP %/mbar   TI min')
    for sp in sps or [1, 1.5, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25]:
        kp, ti = gains_for(table, sp)
        print(f'{sp:7.1f}   {kp:9.2f}   {ti:6.2f}')


def cmd_show(a):
    show(load_table(a.table), sps=a.sp)


def cmd_fit(a):
    with open(a.file, newline='') as f:
        rows = list(csv.DictReader(f))
    t = [float(r['t_s']) for r in rows]
    p0 = float(rows[0]['p0_mbar'])
    dx = float(rows[0]['valve_to']) - float(rows[0]['valve_from'])
    fit = fit_step(t, [float(r['p_mbar']) - p0 for r in rows], dx)
    kp, ti = simc(fit['K'], max(fit['delay'], 1.0), max(fit['tau'], 1.0), a.tc)
    print(f'K {fit["K"]:.4f} mbar/%  delay {fit["delay"]:.1f} s  tau {fit["tau"]:.1f} s  '
          f'(change {fit["dp_end"]:.3f} mbar, rms {fit["rms"]:.4f}) -> KP {kp:.2f} TI {ti:.2f}')


# ---------------------------------------------------------------------------
# run
# ---------------------------------------------------------------------------

def stamp():
    return time.strftime('%Y-%m-%d %H:%M:%S')


def cmd_run(a):
    table = load_table(a.table)
    link = Link(a.host)
    print(f'{stamp()} table {a.table}: ' + ', '.join(f'{sp:g} mbar KP {kp:g} TI {ti:g}' for sp, kp, ti in table),
          flush=True)
    last_sp, said = None, ''
    while True:
        try:
            sp_part, mode_part, pid_part = link.ask('PRES SP?;MODE?;PID?')
            mode_part = mode_part.partition(' ')[2]
            sp = value(sp_part, 'PRES SP')
            kp_now, ti_now = [float(v) for v in pid_part.split()[1].split(',')[:2]]
            if sp is not None and sp != last_sp:
                kp, ti = gains_for(table, sp)
                if (kp, ti) != (kp_now, ti_now):
                    if a.dry_run:
                        print(f'{stamp()} setpoint {sp:g} mbar ({mode_part}): would set KP {kp:.2f} TI {ti:.2f} '
                              f'(now {kp_now:.2f}, {ti_now:.2f})', flush=True)
                    else:
                        link.ask(f'PID SET {kp:.2f},{ti:.2f};PID?')
                        print(f'{stamp()} setpoint {sp:g} mbar ({mode_part}): KP {kp_now:.2f} -> {kp:.2f}, '
                              f'TI {ti_now:.2f} -> {ti:.2f}', flush=True)
                last_sp = sp
            said = ''
        except Refused as e:
            msg = str(e)
            if msg.startswith('ERR 100'):
                sys.exit(f'{stamp()} the board has no PID commands ({msg}); they are in firmware 0.15.8 on')
            if msg != said:
                print(f'{stamp()} refused: {msg} (trying again)', flush=True)
                said = msg
        except (OSError, ConnectionError, ValueError, IndexError) as e:
            if str(e) != said:
                print(f'{stamp()} link: {e} (reconnecting)', flush=True)
                said = str(e)
            link.close()
            last_sp = None
            time.sleep(5)
            continue
        if a.once:
            return
        time.sleep(a.every)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)

    t = sub.add_parser('tune', help='step tests at a few setpoints -> table')
    t.add_argument('host', help='board IP, or IP:port')
    t.add_argument('--sp', type=float, nargs='+', required=True, help='setpoints to test, mbar')
    t.add_argument('--step', type=float, default=0.5,
                   help='first valve step, %% (default 0.5); later setpoints aim at a 10 %% change')
    t.add_argument('--out', default='gains.csv', help='table to write (default gains.csv)')
    t.add_argument('--log', help='folder for the recorded steps (default <out>_steps)')
    t.add_argument('--tc', type=float, default=1.0,
                   help='closed-loop time as a multiple of the delay; larger is calmer (default 1)')
    t.add_argument('--hold', type=float, default=120, help='s the pressure must hold in AUTO first (default 120)')
    t.add_argument('--settle-max', type=float, default=1200, help='s to wait for AUTO to hold (default 1200)')
    t.add_argument('--step-max', type=float, default=900, help='longest step record, s (default 900)')
    t.add_argument('--step-limit', type=float, default=5.0, help='largest valve step, %% (default 5)')
    t.add_argument('--min-dp', type=float, default=0.05,
                   help='smallest usable change, fraction of the setpoint (default 0.05)')
    t.add_argument('--max-dp', type=float, default=0.3,
                   help='largest allowed change, fraction of the setpoint, at least 0.3 mbar (default 0.3)')
    t.add_argument('--speed', type=float, default=1.0, help=argparse.SUPPRESS)   # simulator only
    t.set_defaults(fn=cmd_tune)

    s = sub.add_parser('show', help='the blended KP and TI from a table')
    s.add_argument('table')
    s.add_argument('--sp', type=float, nargs='+', help='setpoints to show')
    s.set_defaults(fn=cmd_show)

    r = sub.add_parser('run', help='keep the board KP and TI matched to its setpoint')
    r.add_argument('host', help='board IP, or IP:port')
    r.add_argument('table')
    r.add_argument('--every', type=float, default=2.0, help='poll period, s (default 2)')
    r.add_argument('--dry-run', action='store_true', help='print what it would set, set nothing')
    r.add_argument('--once', action='store_true', help=argparse.SUPPRESS)   # tests
    r.set_defaults(fn=cmd_run)

    f = sub.add_parser('fit', help='refit one recorded step')
    f.add_argument('file')
    f.add_argument('--tc', type=float, default=1.0)
    f.set_defaults(fn=cmd_fit)

    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == '__main__':
    main()
