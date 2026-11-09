#!/usr/bin/env python3
"""Control test GUI for the pressure controller, in a web browser.

    python pmc_gui.py 192.168.1.100                    # then open http://localhost:8080
    python pmc_gui.py 192.168.1.100 --ls336 192.168.1.165

A small web server on this PC talks to the board's text port (TCP 8888), and
to a Lake Shore 336 (TCP 7777) when one is given, and serves one page:
  Live      pressure, setpoint, valve and the 336 temperatures with charts;
            setpoint, AUTO / MANUAL, Go to %, STOP; the PID gains (PID?,
            PID SET); the logger; the 336 connection.
  Tests     1. valve step test in MANUAL: the plant's gain, delay and time
            constant, and the KP and TI they suggest; 2. setpoint step test
            in AUTO through a list of setpoints (and back): time to 90 %,
            overshoot and settling time of each step.
  Gains     the autotune for the board's PID table (pmc_autotune.py: a relay
            test per setpoint for Ku and Pu, KP = Ku / 4, TI = 2 Pu, a
            checking step, the three sets, Write to board); the gain table
            over several setpoints (pmc_gains.py tune), its
            blended KP and TI (show), and the gain schedule (run), which
            sets KP and TI each time the setpoint changes.
  Board     the event log (EVNT?), reset and OK for alarms, the board clock
            from this PC, and a command line for any text port command.
A bar under the tabs shows the test that is running: its steps, the stage it
is in, how long it has taken and about how long is left.

Everything is saved as it happens, in pmc-logs in the folder the GUI is
started from (--log-dir to change it):
  pmc_<date>_<time>.csv    every sample, one a second, with the 336
                           temperatures and the test stage (the logger);
  <test>_<date>_<time>/    one folder per test: its samples, its results
                           after every step, its messages, and for a gain
                           table the table after every setpoint and each
                           valve step's raw data. A run that stops, stalls
                           or loses the link keeps everything up to there.
On Windows the GUI keeps the PC from sleeping on its own while it runs
(--allow-sleep to let it), and notes any gap in the samples.

Python 3.8 or later, standard library only: nothing to install, and no
admin rights needed. Keep tools/pmc-gui and tools/gain-schedule side by side
(the step fit and the tuner are in pmc_gains.py). The board takes one
text-port connection at a time, so close PuTTY, the logger and
pmc_gains.py first. Set commands need Remote control On.
"""
import argparse
import csv
import datetime
import io
import json
import os
import queue
import re
import socket
import statistics
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
for d in (os.path.join(HERE, '..', 'gain-schedule'), HERE):
    if os.path.exists(os.path.join(d, 'pmc_gains.py')):
        sys.path.insert(0, d)
        break
import pmc_gains   # noqa: E402
import pmc_autotune   # noqa: E402
from pmc_gains import fit_step, simc, gains_for, load_table, Refused   # noqa: E402

BOARD_PORT = 8888
LS_PORT = 7777
HISTORY = 4 * 3600          # samples kept for the page (1 a second): 4 hours
SHOW_SP = [1, 1.5, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25, 35, 50]
POINT_GUESS_S = 900         # a gain table setpoint, until one has finished
AUTO_GUESS_S = 1200         # an autotune setpoint, until one has finished
RIDE_S = 120                # a gain table run waits this long for the link
SETTLED_S = 60              # setpoint step test: held this long within the band = settled
SETTLE_PCT = 5.0            # its band: this % of the step, or the Band box if larger
FIRST_MAX_S = 600           # the most it waits for the first setpoint to hold
GAP_S = 60                  # no sample for this long: noted in the event log
COLS = ['t_s', 'sp_mbar', 'p_mbar', 'valve_pct', 'mode', 'alarm', 'stage']
# The 336 inputs on this rig, used when an input is given without a name.
LS_NAMES = {'A': 'Heat exchanger', 'D': 'Cold head'}
LS_INPUT = re.compile(r'^[A-D][1-5]?$')


def num(part):
    w = part.split()
    try:
        return float(w[-2] if w[-1] in ('mbar', '%') else w[-1])
    except (ValueError, IndexError):
        return None


def kelvin(text):
    """'+1.6234E+00' or '+001.623' to a float; None for no reading."""
    try:
        v = float(text)
    except ValueError:
        return None
    return v if v > 0 else None


def stamp():
    return time.strftime('%Y%m%d_%H%M%S')


def ls_inputs(specs):
    """['A=Heat exchanger', 'D'] or 'A=Heat exchanger, D' to [(input, name)]."""
    if isinstance(specs, str):
        specs = specs.split(',') if ',' in specs or '=' in specs else specs.split()
    out = []
    for s in specs:
        i, _, name = s.partition('=')
        i = i.strip().upper()
        if not i:
            continue
        if not LS_INPUT.match(i):
            raise ValueError('336 inputs are A to D (D1 to D5 with the 3062 card), not "%s"' % i)
        out.append((i, name.strip() or LS_NAMES.get(i, 'Input ' + i)))
    return out


def ls_col(i, name):
    slug = re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')
    return '%s_%s_K' % (i, slug) if slug else '%s_K' % i


def step_band(p0, p1, band):
    """Settled = within 5 % of the step of the new setpoint (as in the
    5 Nov analysis), or within the Band box if that is wider."""
    return max(band, abs(p1 - p0) * SETTLE_PCT / 100)


def metrics(rows, p0, p1, band):
    """rows: (s from the step, pressure). The step response figures."""
    step = p1 - p0
    if abs(step) < 1e-6 or not rows:
        return None
    band = step_band(p0, p1, band)
    t90 = next((t for t, p in rows if (p - p0) / step >= 0.9), None)
    peak = max((p - p0) / step for _, p in rows)
    settle = 0.0
    for t, p in rows:
        if abs(p - p1) > band:
            settle = t
    last = [p for t, p in rows if t >= rows[-1][0] - 60]
    return {'from': p0, 'to': p1, 't90_s': None if t90 is None else round(t90),
            'overshoot_pct': round(max(0.0, (peak - 1) * 100), 1),
            'settle_s': round(settle) if settle < rows[-1][0] else None, 'band': round(band, 3),
            'mean': round(statistics.mean(last), 3),
            'spread': round(statistics.pstdev(last), 3) if len(last) > 1 else 0.0}


class Stopped(Exception):
    """Stop test or STOP pressed while the tuner was running."""


class CsvFile:
    """A CSV written a row at a time and flushed every row, so whatever has
    been written is on disk if the PC sleeps, the GUI is closed or a run
    stalls."""

    def __init__(self, path, header):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.path, self.header = path, header
        self.f = open(path, 'w', newline='', encoding='utf-8')
        self.w = csv.writer(self.f)
        self.w.writerow(header)
        self.f.flush()
        self.rows = 0

    def write(self, row):
        self.w.writerow(['' if x is None else x for x in row])
        self.f.flush()
        self.rows += 1

    def close(self):
        self.f.close()


def pc_time():
    return datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')


class Logger:
    """Every sample to pmc_<date>_<time>.csv. A new file starts when Start
    logging or New file is pressed, or when the 336 inputs change."""

    def __init__(self, folder):
        self.folder = os.path.abspath(folder)
        self.on = True
        self.file = None
        self.last, self.err = '', ''
        self.lock = threading.Lock()

    def write(self, cols, row):
        with self.lock:
            if not self.on:
                return
            try:
                if self.file is None or self.file.header[1:] != cols:
                    self.close()
                    path = os.path.join(self.folder, 'pmc_%s.csv' % stamp())
                    self.file = CsvFile(path, ['time'] + cols)
                now = pc_time()
                self.file.write([now] + row)
                self.last, self.err = now[11:], ''
            except OSError as e:
                self.err = str(e)
                self.close()

    def close(self):
        if self.file:
            self.file.close()
        self.file = None

    def set(self, on=None, new=False):
        with self.lock:
            if new or on is not None:
                self.close()
            if on is not None:
                self.on = on

    def view(self):
        f = self.file
        return {'on': self.on, 'path': f.path if f else '', 'rows': f.rows if f else 0,
                'last': self.last, 'err': self.err, 'folder': self.folder}


class LS336:
    """A Lake Shore 336 on its Ethernet command port, read once a second by
    its own thread, so the temperatures carry on while the board's link is
    down (and the other way round)."""

    def __init__(self, board):
        self.b = board
        self.host, self.inputs, self.gen = '', [], 0
        self.state, self.idn, self.temps = 'off', '', None
        self.sock, self.buf = None, b''
        threading.Thread(target=self.run, daemon=True).start()

    def point(self, host, inputs):
        """Connect to host (empty: disconnect) and read inputs [(input, name)]."""
        host = host.strip()
        if host and not inputs:
            return 'give at least one input, e.g. A=Heat exchanger, D=Cold head'
        self.host, self.inputs = host, inputs
        self.temps, self.idn = None, ''
        self.state = 'connecting' if host else 'off'
        self.gen += 1
        self.b.note('Lake Shore 336: %s' % (
            '%s, %s' % (host, ', '.join('%s %s' % x for x in inputs)) if host else 'off'))
        return ''

    def ask(self, line):
        self.sock.sendall(line.encode() + b'\r\n')
        while b'\n' not in self.buf:
            d = self.sock.recv(1024)
            if not d:
                raise ConnectionError('the 336 closed the connection')
            self.buf += d
        r, self.buf = self.buf.split(b'\n', 1)
        return r.decode(errors='replace').strip()

    def close(self):
        if self.sock:
            self.sock.close()
        self.sock = None

    def run(self):
        while True:
            gen, host, inputs = self.gen, self.host, list(self.inputs)
            if not host:
                time.sleep(0.2)
                continue
            try:
                h, _, p = host.partition(':')
                self.sock = socket.create_connection((h, int(p or LS_PORT)), timeout=5)
                self.buf = b''
                idn = self.ask('*IDN?')
                if gen == self.gen:
                    self.idn, self.state = idn, 'connected'
                    self.b.note('Lake Shore 336 connected: ' + idn)
                nxt = time.monotonic()
                while gen == self.gen:
                    vals = [kelvin(self.ask('KRDG? ' + i)) for i, _ in inputs]
                    if gen != self.gen:
                        break
                    self.temps = (time.monotonic(), vals)
                    self.b.temps_in(vals)
                    nxt = max(nxt + 1.0, time.monotonic() - 1.0)
                    time.sleep(max(0.0, nxt - time.monotonic()))
            except (OSError, ValueError) as e:
                if gen == self.gen:
                    if self.state != 'no link: %s' % e:
                        self.b.note('Lake Shore 336 no link: %s; trying again every 5 s' % e)
                    self.state = 'no link: %s' % e
                    self.temps = None
                self.close()
                end = time.monotonic() + 5
                while time.monotonic() < end and gen == self.gen:
                    time.sleep(0.2)
            self.close()

    def now(self):
        """The latest reading per input, None when older than 5 s; [] when off."""
        if not self.host:
            return []
        t = self.temps
        if t and time.monotonic() - t[0] < 5 and len(t[1]) == len(self.inputs):
            return list(t[1])
        return [None] * len(self.inputs)

    def view(self):
        return {'host': self.host, 'inputs': [list(x) for x in self.inputs], 'state': self.state,
                'idn': self.idn, 'temps': self.now()}


class ViaBoard:
    """pmc_gains.Link's ask/do, through the GUI's one connection."""

    def __init__(self, board):
        self.b = board

    def ask(self, line):
        # A short drop of the link (a cable knocked, the switch, the board's
        # Ethernet restarting) should not end a run of hours: wait up to
        # RIDE_S for the reconnect, then send the line again. Every line a
        # run sends is a query or a set to an absolute value, so sending it
        # twice does no harm.
        give_up = time.monotonic() + RIDE_S / self.b.speed
        try:
            while True:
                if self.b.stop_job:
                    raise Stopped()
                r = self.b.raw(line)
                if not r.startswith('LINK '):
                    break
                if self.b.link_wait is None:
                    self.b.link_wait = time.monotonic()
                if time.monotonic() > give_up:
                    raise ConnectionError('%s (no link for %d s)' % (r[5:], RIDE_S))
                time.sleep(1 / self.b.speed)
        finally:
            self.b.link_wait = None
        parts = r.split(';')
        if parts[0].startswith('ERR'):
            raise Refused(parts[0])
        return parts

    def do(self, cmd):
        return self.ask(cmd + ';MODE?')

    def close(self):
        pass


class GuiTuner(pmc_gains.Tuner):
    """pmc_gains.Tuner that tells the page which stage it is in and saves
    each valve step's fit as soon as it has one."""

    def say(self, msg):
        self.board.note('gain table: ' + msg)

    def hold_auto(self, sp):
        self.board.stage('AUTO at %g mbar, waiting for the pressure to hold for %d s' % (sp, self.a.hold),
                         '%g mbar hold AUTO' % sp, self.a.settle_max)
        self.need_auto = True
        try:
            return super().hold_auto(sp)
        finally:
            self.need_auto = False

    def samples(self, seconds, stop=None):
        # While it waits for AUTO to hold, a fault or warning that drops
        # the board to MANUAL ends that setpoint at once, rather than after
        # the full 20 min wait.
        if not getattr(self, 'need_auto', False):
            return super().samples(seconds, stop)

        def check(rows):
            if rows[-1][3] != 'AUTO':
                alarm = self.board.hist[-1][5] if self.board.hist else ''
                raise RuntimeError('the board left AUTO (%s %s)' % (rows[-1][3], alarm))
            return bool(stop and stop(rows))
        return super().samples(seconds, check)

    def steady(self, sp, seconds_min=60, seconds_max=600):
        self.board.stage('MANUAL, valve still, waiting for a steady baseline', '%g mbar baseline' % sp,
                         seconds_max)
        return super().steady(sp, seconds_min, seconds_max)

    def one_step(self, sp, x_from, x_to, p0, max_dp, name):
        self.board.stage('valve %.1f -> %.1f %%, recording until the response settles' % (x_from, x_to),
                         '%g mbar valve %.1f->%.1f %%' % (sp, x_from, x_to), self.a.step_max)
        fit, away = super().one_step(sp, x_from, x_to, p0, max_dp, name)
        f = fit or {}
        self.board.result('steps.csv', {
            'sp_mbar': sp, 'valve_from': x_from, 'valve_to': x_to, 'p0_mbar': round(p0, 3),
            'dp_mbar': round(f['dp_end'], 3) if f else None, 'K_mbar_per_pct': round(f['K'], 4) if f else None,
            'delay_s': round(f['delay'], 1) if f else None, 'tau_s': round(f['tau'], 1) if f else None,
            'note': 'moved too far' if away else '' if f else 'no fit'})
        return fit, away


class GuiAutoTuner(pmc_autotune.AutoTuner):
    """pmc_autotune.AutoTuner that tells the page which stage it is in."""

    def say(self, msg):
        self.board.note('autotune: ' + msg)

    def stage(self, text, phase=None, most=None):
        self.board.stage(text, phase, most)

    def show(self):
        with self.board.lock:
            if self.board.at is not None:
                self.board.at['calc'], self.board.at['calc_now'] = list(self.calcs), self.calc_now

    def calc(self, text):
        super().calc(text)
        if self.log_dir:     # the working, as it was worked out
            try:
                with open(os.path.join(self.log_dir, 'working.txt'), 'a', encoding='utf-8') as f:
                    f.write(pc_time() + '  ' + text + '\n')
            except OSError:
                pass

    def left(self, want, mode):
        alarm = self.board.hist[-1][5] if self.board.hist else ''
        return 'the board left %s (now %s%s)' % (want, mode or 'no mode',
                                                  ', ' + alarm if alarm and alarm != 'NONE' else '')


class Board:
    """One connection, owned by one thread: it polls once a second and runs
    the lines the page and the tuner hand it through a queue in between."""

    def __init__(self, host, speed=1.0, log_dir='pmc-logs'):
        h, _, p = host.partition(':')
        self.addr = (h, int(p or BOARD_PORT))
        self.speed = speed
        self.sock, self.buf = None, b''
        self.lock = threading.Lock()
        self.tlock = threading.RLock()     # the step tests' state: poll thread and page
        self.cmds = queue.Queue()
        self.hist = []            # COLS + one per 336 input
        self.t0 = time.time()
        self.last_wall = None     # PC time of the last sample, to see a gap (the PC asleep)
        self.state = {'link': 'connecting', 'idn': '', 'pid': None, 'log': []}
        self.logger = Logger(log_dir)
        self.ls = None
        self.test = None          # setpoint step test in progress
        self.results = []
        self.vtest = None         # valve step test in progress
        self.vresults = []
        self.job = None           # 'gain table' while the tuner runs
        self.stop_job = self.abort = False
        self.link_wait = None
        self.table, self.table_rows, self.table_name = None, [], ''
        self.schedule, self.sched_sp = False, None
        self.prog, self.last_prog = None, None    # what the run bar shows
        self.phase = ''           # the stage column of every sample
        self.run_dir, self.run_files, self.run_log = None, {}, None
        self.at = None            # the last autotune: its rows, the three sets, written or not

    def now(self):
        return round((time.time() - self.t0) * self.speed, 2)

    # Link ----------------------------------------------------------------
    def connect(self):
        self.sock = socket.create_connection(self.addr, timeout=5)
        self.buf = b''
        # After a drop the board can still send the answer to a line from
        # the old connection down this new one. Throw away whatever comes in
        # the first half second, then skip any line that is not the *IDN?
        # answer, so every later reply lines up with its question.
        self.sock.settimeout(0.5)
        try:
            while self.sock.recv(4096):
                pass
        except socket.timeout:
            pass
        self.sock.settimeout(5)
        self.sock.sendall(b'*IDN?\n')
        for _ in range(6):
            r = self.line()
            if r.count(',') >= 3 and ';' not in r and not r.startswith('ERR'):
                break
        else:
            raise ConnectionError('no *IDN? answer after connecting')
        self.state['idn'] = r
        self.state['link'] = 'connected'

    def line(self):
        while b'\r\n' not in self.buf:
            d = self.sock.recv(4096)
            if not d:
                raise ConnectionError('the board closed the connection')
            self.buf += d
        r, self.buf = self.buf.split(b'\r\n', 1)
        return r.decode(errors='replace')

    def ask(self, line):
        self.sock.sendall(line.encode() + b'\n')
        return self.line()

    def queued(self, kind, line, wait=15):
        done = queue.Queue()
        self.cmds.put((kind, line, done))
        try:
            return done.get(timeout=wait)
        except queue.Empty:
            return 'LINK no reply from the board'

    def send(self, line):
        """A set command: '' when taken, else the ERR."""
        return self.queued('set', line)

    def raw(self, line):
        """Any line: the reply as it came."""
        return self.queued('raw', line)

    def note(self, text):
        with self.lock:
            self.state['log'] = (self.state['log'] + [time.strftime('%H:%M:%S ') + text])[-200:]
            if self.run_log:
                try:
                    self.run_log.write(pc_time() + '  ' + text + '\n')
                    self.run_log.flush()
                except (OSError, ValueError):
                    pass

    # Loop ----------------------------------------------------------------
    def run(self):
        while True:
            try:
                if self.sock is None:
                    self.connect()
                    self.note('connected: ' + self.state['idn'])
                self.serve()
            except (OSError, ConnectionError) as e:
                if self.state['link'] != 'no link: %s' % e:
                    self.note('no link: %s; trying again every 2 s' % e)
                self.state['link'] = 'no link: %s' % e
                if self.sock:
                    self.sock.close()
                self.sock = None
                self.flush('LINK %s' % e)
                time.sleep(2 / self.speed)

    def flush(self, reply):
        while not self.cmds.empty():
            self.cmds.get()[2].put(reply)

    def serve(self):
        nxt = time.monotonic()
        while True:
            wait = nxt - time.monotonic()
            if wait <= 0:
                self.poll()
                nxt = max(nxt + 1.0 / self.speed, time.monotonic())
                continue
            try:
                kind, line, done = self.cmds.get(timeout=wait)
            except queue.Empty:
                continue
            try:
                done.put(self.job_line(kind, line))
            except (OSError, ConnectionError) as e:
                done.put('LINK %s' % e)
                raise

    def job_line(self, kind, line):
        if kind == 'events':
            first = self.ask(line)
            if not first.startswith('EVNT '):
                return [first]
            return [first] + [self.line() for _ in range(int(first.split()[1]))]
        if kind == 'raw':
            return self.ask(line)
        r = self.ask(line + ';MODE?')     # a query, so every line gets one reply
        err = [x for x in r.split(';') if x.startswith('ERR')]
        self.note('%s  %s' % (line, err[0] if err else 'ok'))
        return err[0] if err else ''

    def cols(self):
        return COLS + [ls_col(i, n) for i, n in (self.ls.inputs if self.ls and self.ls.host else [])]

    def add_row(self, row):
        now = time.time()
        if self.last_wall is not None and (now - self.last_wall) * self.speed > GAP_S:
            gap = int(now - self.last_wall)
            self.note('no samples for %d h %02d min, %s to %s: was this PC asleep, or both links down?' % (
                gap // 3600, gap % 3600 // 60, time.strftime('%a %H:%M', time.localtime(self.last_wall)),
                time.strftime('%a %H:%M', time.localtime(now))))
        self.last_wall = now
        with self.lock:
            self.hist.append(row)
            del self.hist[:-HISTORY]
            cols = self.cols()
            f = self.run_files.get('samples')
        self.logger.write(cols, row)
        if f:
            try:
                f.write([pc_time()] + row)
            except (OSError, ValueError):
                pass

    def temps_in(self, vals):
        """A 336 reading. With no board link, it makes the sample itself."""
        if self.sock is None:
            self.add_row([self.now(), None, None, None, '', '', self.phase] + vals)

    def poll(self):
        # SCHED? and PIDS? last: firmware before 0.15.15 refuses them (ERR 100),
        # which ends the line there.
        r = self.ask('PRES SP?;PRES?;VALV?;MODE?;ALRM?;PID?;SCHED?;PIDS?')
        sp, p, v, m, a, pid, sch, pids = (r.split(';') + [''] * 8)[:8]
        row = [self.now(), num(sp), num(p), num(v), m[5:], a[5:], self.phase] + (self.ls.now() if self.ls else [])
        self.add_row(row)
        if pid.startswith('PID '):
            with self.lock:
                self.state['pid'] = [float(x) for x in pid[4:].split(',')]
        # The board's own gain schedule (0.15.15): which set PID? is, and on what.
        with self.lock:
            self.state['board_sched'] = sch[6:].split(',')[0] if sch.startswith('SCHED ') else None
            self.state['pid_set'] = int(pids[5:].split(',')[0]) if pids.startswith('PIDS ') else None
        self.schedule_tick(row)      # first: a step below sets its own setpoint's gains
        with self.tlock:
            if self.test:
                self.step_tick(row)
            if self.vtest:
                self.vstep_tick(row)

    def busy(self):
        return self.test or self.vtest or self.job

    # A test's run: its folder, the run bar ----------------------------------
    def begin(self, kind, name, labels, durs, settings):
        """A test starts: its folder (samples, messages), and the run bar."""
        t = self.now()
        d = os.path.join(self.logger.folder, '%s_%s' % (kind, stamp()))
        with self.lock:
            self.prog = {'kind': kind, 'name': name, 't0': t, 'durs': durs, 'i': None,
                         'steps': [{'label': x, 'state': 'todo', 'note': '', 't0': None, 't1': None} for x in labels],
                         'stage': '', 'stage_t': t, 'stage_max': None, 'dir': d}
            try:
                self.run_files = {'samples': CsvFile(os.path.join(d, 'samples.csv'), ['time'] + self.cols())}
                self.run_log = open(os.path.join(d, 'messages.txt'), 'w', encoding='utf-8')
                self.run_dir = d
            except OSError as e:
                self.run_files, self.run_log, self.run_dir = {}, None, None
                self.prog['dir'] = ''
                self.state['log'].append(time.strftime('%H:%M:%S ') + 'could not make %s: %s' % (d, e))
        self.note('%s started: %s' % (name, settings))

    def step(self, i, state, note=''):
        t = self.now()
        with self.lock:
            P = self.prog
            if not P:
                return
            s = P['steps'][i]
            s['state'] = state
            if note:
                s['note'] = note
            if state == 'now':
                s['t0'], P['i'] = t, i
            else:
                s['t1'] = t

    def stage(self, text, phase=None, most=None, restart=True):
        with self.lock:
            if self.prog:
                self.prog['stage'] = text
                if restart:
                    self.prog['stage_t'], self.prog['stage_max'] = self.now(), most
            if phase is not None:
                self.phase = phase

    def result(self, name, d):
        """A result row to <test folder>/<name>, written as it comes."""
        with self.lock:
            f = self.run_files.get(name)
            if f is None and self.run_dir:
                try:
                    f = self.run_files[name] = CsvFile(os.path.join(self.run_dir, name), ['time'] + list(d))
                except OSError:
                    return
        if f:
            try:
                f.write([pc_time()] + list(d.values()))
            except (OSError, ValueError):
                pass

    @staticmethod
    def where(P):
        """A step that did not finish says the stage it was in."""
        for s in P['steps']:
            if s['state'] in ('now', 'stopped') and not s['note'] and P['stage']:
                s['note'] = 'stopped during: ' + P['stage']

    def end(self, outcome):
        self.note('%s %s' % (self.prog['name'] if self.prog else 'test', outcome))
        with self.lock:
            P = self.prog
            if P:
                for s in P['steps']:
                    if s['state'] == 'now':
                        s['state'] = 'done' if outcome == 'done' else 'stopped'
                if outcome != 'done':
                    self.where(P)
                self.last_prog = {'name': P['name'], 'outcome': outcome, 'at': time.strftime('%H:%M'),
                                  'steps': self.steps_view(P['steps']), 'dir': P['dir']}
            self.prog, self.phase = None, ''
            for f in self.run_files.values():
                f.close()
            if self.run_log:
                self.run_log.close()
            self.run_dir, self.run_files, self.run_log = None, {}, None

    def steps_view(self, steps):
        t = self.now()
        return [{'label': s['label'], 'state': s['state'], 'note': s['note'],
                 'took_s': (s['t1'] if s['t1'] is not None else t) - s['t0'] if s['t0'] is not None else None}
                for s in steps]

    def prog_view(self):
        """The run bar: the steps, the stage, elapsed and about how long is left."""
        P = self.prog
        if not P:
            return None
        t = self.now()
        i = P['i']
        cur = P['steps'][i] if i is not None else None
        in_step = t - cur['t0'] if cur and cur['t0'] is not None else 0.0
        how = ''
        if P['kind'] in ('gains', 'autotune'):
            pts = P['steps'][:-1] if P['kind'] == 'autotune' else P['steps']   # not its last step, the sets
            took = [s['t1'] - s['t0'] for s in pts if s['state'] == 'done' and s['t0'] is not None]
            per = sum(took) / len(took) if took else POINT_GUESS_S if P['kind'] == 'gains' else AUTO_GUESS_S
            todo = sum(1 for s in pts if s['state'] == 'todo')
            left, how = max(per - in_step, 60) + per * todo, 'about'
        else:
            durs = P['durs']
            rest = sum(d for d, s in zip(durs, P['steps']) if s['state'] == 'todo')
            left = rest + (max(durs[i] - in_step, 0) if i is not None else 0)
            if P['kind'] == 'setpoint':      # each step can end early, once settled
                how = 'at most'
        wait = None
        if self.link_wait is not None:
            wait = 'waiting for the board link: %d of %d s' % ((time.monotonic() - self.link_wait) * self.speed, RIDE_S)
        steps = self.steps_view(P['steps'])
        return {'kind': P['kind'], 'name': P['name'], 'i': i, 'steps': steps, 'stage': P['stage'],
                'stage_s': t - P['stage_t'], 'stage_max': P['stage_max'], 'elapsed_s': t - P['t0'],
                'left_s': left, 'how': how, 'wait': wait, 'dir': P['dir'],
                'done_at': time.strftime('%H:%M', time.localtime(time.time() + left / self.speed))}

    # Setpoint step test (AUTO) -------------------------------------------
    # AUTO at the first setpoint until it holds within the band, then a step
    # to each next one in turn (and back down through them when asked). A
    # step ends once the pressure has held within 5 % of the step (or the
    # band) for 1 min, or at the limit. The gain schedule, when on, sets the
    # gains for each new setpoint just before its step.
    def start_test(self, sps, back, record, hold, band):
        if self.busy():
            return 'a test is already running'
        sps = [round(x, 1) for x in sps]
        if len(sps) < 2 or any(x <= 0 for x in sps) or any(a == b for a, b in zip(sps, sps[1:])):
            return 'give two or more setpoints above 0, each different from the one before'
        r = self.send('PRES SP %.1f;MODE AUTO' % sps[0])
        if r:
            return r
        pairs = list(zip(sps, sps[1:]))
        if back:
            pairs += [(b, a) for a, b in reversed(pairs)]
        labels = ['Settle at %.1f mbar' % sps[0]] + ['Step %.1f -> %.1f mbar' % x for x in pairs]
        self.begin('setpoint', 'Setpoint step test', labels, [FIRST_MAX_S] + [record] * len(pairs),
                   '%s mbar%s, each step until it holds %d s within %g %% of the step (band %.2f mbar) '
                   'or %d s at most, gain schedule %s' %
                   (' -> '.join('%g' % x for x in sps), ' and back' if back else '', SETTLED_S, SETTLE_PCT,
                    band, record, 'on (%s)' % (self.table_name or 'table') if self.schedule and self.table
                    else 'off'))
        with self.tlock:
            self.test = {'start': sps[0], 'pairs': pairs, 'record': record, 'hold': hold, 'band': band,
                         'phase': 'settle', 'since': None, 't_phase': None, 'rows': [], 'steps': [], 'k': 0}
        self.step(0, 'now')
        self.stage('AUTO at %.1f mbar, waiting for it to hold within %.2f mbar for %d s' % (sps[0], band, hold),
                   'settle at %.1f mbar' % sps[0], FIRST_MAX_S)
        return ''

    def stop_test(self, why='stopped', put_back=True):
        with self.tlock:
            if self.test:
                T, self.test = self.test, None
                if put_back:
                    self.cmds.put(('set', 'PRES SP %.1f' % T['start'], queue.Queue()))
                    why += '; setpoint back to %.1f mbar' % T['start']
                self.end(why)

    def step_tick(self, row):
        T = self.test
        t, p, mode = row[0], row[2], row[4]
        if mode != 'AUTO':
            self.stop_test('ended: the board left AUTO (%s %s)' % (mode, row[5]))
            return
        if T['t_phase'] is None:
            T['t_phase'] = t
        if T['phase'] == 'settle':
            if p is not None and abs(p - T['start']) <= T['band']:
                T['since'] = T['since'] if T['since'] is not None else t
                self.stage('AUTO at %.1f mbar, within %.2f mbar for %d of %d s' %
                           (T['start'], T['band'], t - T['since'], T['hold']), restart=False)
                if t - T['since'] >= T['hold']:
                    self.step(0, 'done', 'held %d s' % T['hold'])
                    T['steps'] = list(T['pairs'])
                    self.next_step(t)
            else:
                T['since'] = None
                self.stage('AUTO at %.1f mbar, waiting for it to hold within %.2f mbar for %d s' %
                           (T['start'], T['band'], T['hold']), restart=False)
                if t - T['t_phase'] >= FIRST_MAX_S:
                    # The steps still say how the gains do: go on from where it is.
                    self.note('%.1f mbar not held within %.2f mbar in %d min; stepping anyway' %
                              (T['start'], T['band'], FIRST_MAX_S // 60))
                    self.step(0, 'done', 'not held in %d min, stepped anyway' % (FIRST_MAX_S // 60))
                    T['steps'] = list(T['pairs'])
                    self.next_step(t)
            return
        p0, p1 = T['cur']
        band = step_band(p0, p1, T['band'])
        if p is not None:
            T['rows'].append((t - T['t_phase'], p))
            T['in'] = (T['in'] if T['in'] is not None else t) if abs(p - p1) <= band else None
        held = T['in'] is not None and t - T['in'] >= SETTLED_S
        if T['in'] is not None and not held:
            self.stage('setpoint %.1f -> %.1f mbar, within %.2f mbar for %d of %d s' %
                       (p0, p1, band, t - T['in'], SETTLED_S), restart=False)
        else:
            self.stage('setpoint %.1f -> %.1f mbar, waiting for it to hold within %.2f mbar for %d s' %
                       (p0, p1, band, SETTLED_S), restart=False)
        if held or t - T['t_phase'] >= T['record']:
            m = metrics(T['rows'], p0, p1, T['band'])
            if m:
                pid = self.state['pid'] or [None, None]
                m['kp'], m['ti'] = pid[0], pid[1]
                with self.lock:
                    self.results.append(m)
                self.result('results.csv', m)
                self.note('%.1f -> %.1f mbar, KP %s TI %s: 90 %% in %s s, overshoot %s %%, settled in %s s' %
                          (p0, p1, m['kp'], m['ti'], m['t90_s'], m['overshoot_pct'],
                           m['settle_s'] if held else 'no (%d s limit)' % T['record']))
                note = 'overshoot %s %%, %s' % (m['overshoot_pct'], 'settled in %s s' % m['settle_s'] if held
                                                else 'not settled in %d s' % T['record'])
            else:
                note = ''
            self.step(T['k'], 'done', note)
            if T['steps']:
                self.next_step(t)
            else:
                self.test = None
                self.end('done')

    def next_step(self, t):
        T = self.test
        T['cur'] = T['steps'].pop(0)
        T['phase'] = '%.1f->%.1f mbar' % T['cur']
        T['t_phase'], T['rows'], T['k'], T['in'] = t, [], T['k'] + 1, None
        self.step(T['k'], 'now')
        self.stage('setpoint %.1f -> %.1f mbar, waiting for it to hold within %.2f mbar for %d s' %
                   (T['cur'] + (step_band(*T['cur'], T['band']), SETTLED_S)),
                   'setpoint %.1f->%.1f mbar' % T['cur'], T['record'])
        self.schedule_set(T['cur'][1])                         # the new setpoint's gains first
        r = self.ask('PRES SP %.1f;PRES SP?' % T['cur'][1])   # the query: a set alone gives no reply
        if r.startswith('ERR'):
            self.note('setpoint refused: ' + r)

    # Valve step test (MANUAL) --------------------------------------------
    # The valve to At %, a wait for the pressure to settle, then At + Step %
    # and back, each recorded and fitted to a gain, a delay and a time
    # constant (pmc_gains.fit_step), with the KP and TI the SIMC rules give.
    def start_vtest(self, at, step, hold, record):
        if self.busy():
            return 'a test is already running'
        if not (0 <= at <= 100 and 0 < at + step <= 100 and step):
            return 'At and At + Step must be within 0 to 100 %'
        r = self.send('MODE MANUAL;VALV SET %.1f' % at)
        if r:
            return r
        self.begin('valve', 'Valve step test',
                   ['Settle at %.1f %%' % at, 'Step %.1f -> %.1f %%' % (at, at + step),
                    'Step %.1f -> %.1f %%' % (at + step, at)], [hold, record, record],
                   'at %.1f %%, step %.1f %%, settle %d s, record %d s' % (at, step, hold, record))
        with self.tlock:
            self.vtest = {'at': at, 'step': step, 'hold': hold, 'record': record, 'phase': 'valve settle',
                          't_phase': None, 'rows': [], 'moves': [(at, at + step), (at + step, at)], 'base': [],
                          'k': 0}
        self.step(0, 'now')
        self.stage('MANUAL, valve to %.1f %%, waiting %d s for the pressure to settle' % (at, hold),
                   'valve settle at %.1f %%' % at, hold)
        return ''

    def stop_vtest(self, why='stopped'):
        with self.tlock:
            if self.vtest:
                self.vtest = None
                self.end(why + '; valve left where it is, in MANUAL')

    def vstep_tick(self, row):
        V = self.vtest
        t, p, mode = row[0], row[2], row[4]
        if mode != 'MANUAL':
            self.stop_vtest('ended: the board is in %s %s' % (mode, row[5]))
            return
        if V['t_phase'] is None:
            V['t_phase'] = t
        if p is not None:
            V['base'] = (V['base'] + [p])[-30:]
            V['rows'].append((t - V['t_phase'], p))
        if t - V['t_phase'] < (V['hold'] if V['phase'] == 'valve settle' else V['record']):
            return
        if V['phase'] != 'valve settle':
            x0, x1 = V['cur']
            base = V['p0']
            ts = [a for a, _ in V['rows']]
            dp = [b - base for _, b in V['rows']]
            f = fit_step(ts, dp, x1 - x0)
            if f and f['K'] > 0:
                kp, ti = simc(f['K'], max(f['delay'], 1.0), max(f['tau'], 1.0))
                m = {'from': x0, 'to': x1, 'p0': round(base, 3), 'dp': round(f['dp_end'], 3),
                     'K': round(f['K'], 3), 'delay_s': round(f['delay']), 'tau_s': round(f['tau']),
                     'kp': round(kp, 2), 'ti': round(ti, 2)}
                self.note('valve %.1f -> %.1f %%: pressure %+.3f mbar, K %.3f mbar/%%, delay %d s, tau %d s, '
                          'SIMC KP %.2f TI %.2f' % (x0, x1, m['dp'], m['K'], m['delay_s'], m['tau_s'], m['kp'], m['ti']))
                note = 'K %.3f, KP %.2f, TI %.2f' % (m['K'], m['kp'], m['ti'])
            else:
                m = {'from': x0, 'to': x1, 'p0': round(base, 3), 'dp': round(dp[-1], 3) if dp else None,
                     'K': None, 'delay_s': None, 'tau_s': None, 'kp': None, 'ti': None}
                self.note('valve %.1f -> %.1f %%: no usable response (pressure moved too little, '
                          'or the wrong way); try a larger step' % (x0, x1))
                note = 'no usable response'
            with self.lock:
                self.vresults.append(m)
            self.result('results.csv', m)
            self.step(V['k'], 'done', note)
        else:
            self.step(0, 'done')
        if not V['moves']:
            self.vtest = None
            self.end('done')
            return
        V['cur'] = V['moves'].pop(0)
        V['p0'] = statistics.mean(V['base']) if V['base'] else p
        V['phase'] = 'valve %.1f->%.1f %%' % V['cur']
        V['t_phase'], V['rows'], V['k'] = t, [], V['k'] + 1
        self.step(V['k'], 'now')
        self.stage('valve %.1f -> %.1f %%, recording %d s' % (V['cur'] + (V['record'],)), V['phase'], V['record'])
        r = self.ask('VALV SET %.1f;VALV?' % V['cur'][1])
        if r.startswith('ERR'):
            self.stop_vtest('ended: ' + r)

    # Gain table (pmc_gains.py tune) ----------------------------------------
    def start_tune(self, sps, step, tc):
        if self.busy():
            return 'a test is already running'
        sps = sorted(set(s for s in sps if s > 0))
        if not sps:
            return 'give at least one setpoint'
        a = SimpleNamespace(hold=120, settle_max=1200, step_max=900, step_limit=5.0, min_dp=0.05,
                            max_dp=0.3, tc=tc, sp=sps, step=step)
        self.job, self.stop_job, self.abort = 'gain table', False, False
        self.begin('gains', 'Gain table', ['%g mbar' % s for s in sps], None,
                   'setpoints %s mbar, first step %.1f %%, tc %g' % (' '.join('%g' % s for s in sps), step, tc))
        threading.Thread(target=self.tune_job, args=(a,), daemon=True).start()
        return ''

    def tune_job(self, a):
        link = ViaBoard(self)
        t = GuiTuner(link, pmc_gains.Clock(self.speed), a, self.run_dir)
        t.board, t.x0 = self, None
        sp0 = mode0 = None
        rows, step, outcome, skipped = [], a.step, 'done', []
        try:
            sp0 = pmc_gains.value(link.ask('PRES SP?')[0], 'PRES SP')
            mode0 = t.sample()[2]
            t.say('found setpoint %s mbar, %s; testing %s mbar' % (sp0, mode0, ', '.join('%g' % s for s in a.sp)))
            for i, sp in enumerate(a.sp):
                self.step(i, 'now')
                try:
                    r = t.tune_point(sp, step)
                except RuntimeError as e:
                    # One setpoint that gives no usable step (too little or
                    # too much response) should not lose the others.
                    t.say('%g mbar skipped: %s' % (sp, e))
                    self.step(i, 'failed', str(e))
                    skipped.append('%g' % sp)
                    continue
                rows.append(r)
                self.step(i, 'done', 'KP %.2f, TI %.2f min' % (r['kp'], r['ti_min']))
                self.set_table([(x['sp_mbar'], x['kp'], x['ti_min']) for x in rows], rows, 'measured')
                self.save_table()
                K = r['K_mbar_per_pct']
                if K > 0 and i + 1 < len(a.sp):
                    step = min(max(round(0.10 * a.sp[i + 1] / K, 1), 0.1), a.step_limit)
            if skipped:
                outcome = 'done, %s mbar skipped' % ', '.join(skipped)
        except Stopped:
            outcome = 'ended by STOP' if self.abort else 'stopped'
        except (Refused, ConnectionError, ValueError) as e:
            outcome = 'ended: %s' % e
        finally:
            self.stop_job = False
            back = []
            if self.abort:
                t.say('STOP pressed: nothing put back')
            else:
                if t.x0 is not None:
                    back.append('VALV SET %.1f' % t.x0)
                if sp0 is not None:
                    back.append('PRES SP %.1f' % sp0)
                if mode0 == 'AUTO':
                    back.append('MODE AUTO')
            if outcome != 'done' and self.prog:
                with self.lock:
                    self.where(self.prog)
            if back:
                self.stage('putting back what it found: ' + ', '.join(back), 'put back')
            for c in back:
                r = self.send(c)
                t.say('put back: %s%s' % (c, ' (' + r + ')' if r else ''))
            self.end(outcome)
            self.job = None

    # Autotune (pmc_autotune.py) ------------------------------------------
    def start_autotune(self, sps, d, div, b1, b2, h, kp0=0.10, ti0=0.40):
        if self.busy():
            return 'a test is already running'
        sps = sorted(set(round(x, 1) for x in sps if x > 0))
        if not sps:
            return 'give at least one setpoint'
        if not 0 < b1 < b2:
            return 'B1 must be above 0 and below B2'
        if not 0 <= h <= 50:
            return 'H must be 0 to 50 %'
        if not 0.1 <= d <= pmc_autotune.D_MAX:
            return 'the relay step must be 0.1 to %g %%' % pmc_autotune.D_MAX
        if not 2 <= div <= 8:
            return 'KP = Ku / must be 2 to 8'
        if not (pmc_gains.KP_MIN <= kp0 <= pmc_gains.KP_MAX and pmc_gains.TI_MIN <= ti0 <= pmc_gains.TI_MAX):
            return 'the first gains must be KP %g to %g and TI %g to %g min' % (
                pmc_gains.KP_MIN, pmc_gains.KP_MAX, pmc_gains.TI_MIN, pmc_gains.TI_MAX)
        r = self.raw('PRES SP?;PRES?;VALV?;ALRM?;PID?')
        if r.startswith('LINK '):
            return r
        parts = (r.split(';') + [''] * 5)[:5]
        if parts[1] == 'PRES NONE':
            return 'no pressure reading'
        if parts[2] == 'VALV NONE':
            return 'the valve is not homed: home it on the screen first'
        if 'F-' in parts[3]:
            return 'reset the fault first (Board tab): ' + parts[3][5:]
        if not parts[4].startswith('PID '):
            return 'this firmware has no PID SET (0.15.8 or later): ' + parts[4]
        r = self.send('PID SET ' + parts[4][4:])   # the same gains, so nothing is written: ERR 211 when Remote control is Off
        if r:
            return r
        a = SimpleNamespace(sps=sps, d=d, div=div, ti_pu=2.0, trims=2, b1=b1, b2=b2, h=h, kp0=kp0, ti0=ti0)
        self.job, self.stop_job, self.abort = 'autotune', False, False
        self.begin('autotune', 'PID autotune', ['%g mbar' % x for x in sps] + ['PID table sets'], None,
                   'setpoints %s mbar, first gains KP %.2f TI %.2f, first relay step %.1f %%, KP = Ku / %g, TI = 2 Pu, '
                   'PID table B1 %g B2 %g H %d %%' % (' '.join('%g' % x for x in sps), kp0, ti0, d, div, b1, b2, h))
        with self.lock:
            self.at = {'rows': [], 'sets': [], 'b1': b1, 'b2': b2, 'h': h, 'dir': self.run_dir, 'lines': [],
                       'written': '', 'running': True, 'calc': [], 'calc_now': '', 'sets_calc': []}
        threading.Thread(target=self.autotune_job, args=(a,), daemon=True).start()
        return ''

    def at_rows(self, rows, a):
        """The results so far: the sets, and the files that hold them."""
        sets = pmc_autotune.table_sets(rows, a.b1, a.b2)
        lines = pmc_autotune.write_lines(sets, a.b1, a.b2, a.h)
        how = []
        for n, kp, ti, pts in sets:
            if kp is None:
                continue
            rs = [r for r in rows if r['sp_mbar'] in pts]
            if len(rs) == 1:
                how.append('Set %d: KP %.2f and TI %.2f min, from %g mbar alone' % (n, kp, ti, pts[0]))
            else:
                how.append('Set %d: KP = geometric mean of %s = %.2f; TI = geometric mean of %s = %.2f min' % (
                    n, ', '.join('%.2f' % r['kp'] for r in rs), kp, ', '.join('%.2f' % r['ti_min'] for r in rs), ti))
        with self.lock:
            self.at.update(rows=list(rows), sets=[list(x) for x in sets], lines=lines, sets_calc=how)
        if not self.run_dir:
            return
        try:
            with open(os.path.join(self.run_dir, 'table.csv'), 'w', newline='', encoding='utf-8') as f:
                w = csv.writer(f)
                w.writerow(['sp_mbar', 'kp', 'ti_min', 'Ku', 'Pu_s'])     # loads as a PC gain table
                for r in rows:
                    w.writerow([r['sp_mbar'], r['kp'], r['ti_min'], r['Ku'], r['Pu_s']])
            with open(os.path.join(self.run_dir, 'sets.csv'), 'w', newline='', encoding='utf-8') as f:
                w = csv.writer(f)
                w.writerow(['set', 'kp', 'ti_min', 'from_mbar', 'b1_mbar', 'b2_mbar', 'h_pct'])
                for n, kp, ti, pts in sets:
                    w.writerow([n, kp, ti, ' '.join('%g' % x for x in pts), a.b1, a.b2, a.h])
            with open(os.path.join(self.run_dir, 'write-to-board.txt'), 'w', encoding='utf-8') as f:
                f.write('\n'.join(lines) + '\n')
        except OSError as e:
            self.note('could not save the autotune results: %s' % e)

    def autotune_job(self, a):
        link = ViaBoard(self)
        t = GuiAutoTuner(link, pmc_gains.Clock(self.speed), a, self.run_dir)
        t.board = self
        f, rows, outcome, skipped = None, [], 'done', []
        last = len(a.sps)
        try:
            f = pmc_autotune.found(link)
            t.say('found setpoint %s mbar, %s, PID %s, PID table %s' %
                  (f['sp'], f['mode'], ', '.join('%.2f' % x for x in f['pid']), f['sched'] or 'not in this firmware'))
            pmc_autotune.pid_table_off(link, f)
            gains = (a.kp0, a.ti0)      # to reach the first point; each later one uses the point before's
            ku = None
            for i, sp in enumerate(a.sps):
                self.step(i, 'now')
                try:
                    r = t.point(sp, gains, pmc_autotune.next_d(sp, ku, a.d))
                except RuntimeError as e:
                    # One point with no usable oscillation (or a warning that
                    # dropped AUTO) should not lose the others.
                    t.say('%g mbar skipped: %s' % (sp, e))
                    self.step(i, 'failed', str(e))
                    skipped.append('%g' % sp)
                    continue
                rows.append(r)
                self.result('results.csv', r)
                self.step(i, 'done', 'Ku %.2f, KP %.2f, TI %.2f%s' % (r['Ku'], r['kp'], r['ti_min'],
                                                                     ', trimmed ' + r['trims'] if r['trims'] else ''))
                self.at_rows(rows, a)
                ku, gains = r['Ku'], (r['kp'], r['ti_min'])
            self.step(last, 'now')
            if rows:
                sets = pmc_autotune.table_sets(rows, a.b1, a.b2)
                self.step(last, 'done', '; '.join('set %d %s' % (n, 'KP %.2f TI %.2f' % (kp, ti) if kp is not None
                                                                   else 'not measured') for n, kp, ti, _ in sets))
            else:
                self.step(last, 'failed', 'no point measured')
            if skipped:
                outcome = 'done, %s mbar skipped' % ', '.join(skipped)
            if not rows:
                outcome = 'ended: no point measured'
        except Stopped:
            outcome = 'ended by STOP' if self.abort else 'stopped'
        except (Refused, ConnectionError, ValueError) as e:
            outcome = 'ended: %s' % e
        finally:
            self.stop_job = False
            with self.lock:
                if self.at:
                    self.at['running'] = False
            if outcome not in ('done',) and not outcome.startswith('done,') and self.prog:
                with self.lock:
                    self.where(self.prog)
            if self.abort:
                t.say('STOP pressed: nothing put back')
            elif f:
                back = pmc_autotune.put_back_lines(f)
                self.stage('putting back what it found: ' + ', '.join(back), 'put back')
                for c in back:
                    r = self.send(c)
                    t.say('put back: %s%s' % (c, ' (' + r + ')' if r else ''))
            self.end(outcome)
            self.job = None

    def write_sets(self):
        """The three sets and the PID table (pressure) to the board."""
        if self.busy():
            return 'a test is running'
        with self.lock:
            at = self.at
            lines = list(at['lines']) if at else []
        if not lines or not any(x[1] is not None for x in at['sets']):
            return 'run the autotune first'
        for line in lines:
            r = self.send(line)
            if r.startswith('ERR 100'):
                return ('this firmware has no PID table (0.15.15 or later); load table.csv from %s as the PC gain '
                        'table instead' % at['dir'])
            if r:
                return '%s: %s' % (line, r)
        with self.lock:
            at['written'] = time.strftime('%H:%M')
        self.note('autotune: PID table written to the board: ' + '; '.join(lines))
        return ''

    def save_table(self):
        if self.run_dir:
            try:
                with open(os.path.join(self.run_dir, 'table.csv'), 'w', newline='', encoding='utf-8') as f:
                    f.write(self.table_csv())
            except OSError as e:
                self.note('could not save the table: %s' % e)

    def set_table(self, table, rows, name):
        with self.lock:
            self.table, self.table_rows, self.table_name = table, rows, name
        self.sched_sp = None   # the schedule applies it at the next poll

    def load_csv(self, text, name):
        with tempfile.NamedTemporaryFile('w', suffix='.csv', delete=False, newline='') as f:
            f.write(text)
        try:
            try:
                table = load_table(f.name)
            except (ValueError, KeyError) as e:
                raise ValueError(str(e).replace(f.name, name or 'table')) from None
            with open(f.name, newline='') as g:
                rows = [r for r in csv.DictReader(g)]
        finally:
            os.unlink(f.name)
        keep = []
        for r in rows:
            try:
                keep.append({k: float(v) for k, v in r.items() if k and v not in (None, '')})
            except ValueError:
                pass
        self.set_table(table, keep, name or 'loaded')
        self.note('gain table %s: %s' % (name, ', '.join('%g mbar KP %g TI %g' % x for x in table)))

    def table_csv(self):
        cols = ['sp_mbar', 'kp', 'ti_min', 'K_mbar_per_pct', 'delay_s', 'tau_s', 'step_pct', 'valve_pct', 'p0_mbar']
        f = io.StringIO()
        w = csv.DictWriter(f, fieldnames=cols, extrasaction='ignore')
        w.writeheader()
        with self.lock:
            for r in self.table_rows:
                w.writerow(r)
        return f.getvalue()

    # Gain schedule (pmc_gains.py run) ------------------------------------
    def schedule_tick(self, row):
        sp = row[1]
        if sp is not None and not self.job and sp != self.sched_sp:
            self.schedule_set(sp)

    def schedule_set(self, sp):
        """With the schedule on: KP and TI from the table for this setpoint,
        sent only when they differ from the board's (each PID SET is a
        flash write)."""
        if not self.schedule or not self.table:
            return
        if self.state.get('board_sched') in ('SP', 'PV'):
            # The board picks its own PID set: a PID SET here would overwrite
            # the set in use.
            self.note('gain schedule off: the board has its own PID table on (SCHED %s)' % self.state['board_sched'])
            self.schedule = False
            return
        kp, ti = gains_for(self.table, sp)
        now = self.state['pid']
        if not now or (round(now[0], 2), round(now[1], 2)) != (kp, ti):
            r = self.ask('PID SET %.2f,%.2f;PID?' % (kp, ti))
            if r.startswith('ERR'):
                self.note('gain schedule off: %s' % r)
                self.schedule = False
                return
            if r.startswith('PID '):
                with self.lock:
                    self.state['pid'] = [float(x) for x in r[4:].split(',')]
            self.note('gain schedule: setpoint %g mbar, KP %.2f TI %.2f' % (sp, kp, ti))
        self.sched_sp = sp

    # STOP ----------------------------------------------------------------
    def stop_all(self):
        """STOP: the valve stops where it is and any test ends, with nothing
        put back (Stop test, by contrast, puts back what the test found)."""
        self.abort = True
        if self.job:
            self.stop_job = True
        self.stop_test('ended by STOP', put_back=False)
        self.stop_vtest('ended by STOP')
        return self.send('STOP')

    def stop_gently(self):
        self.stop_test()
        self.stop_vtest()
        if self.job:
            self.abort, self.stop_job = False, True

    # For the page --------------------------------------------------------
    def snapshot(self, since):
        ls = self.ls.view() if self.ls else None
        with self.lock:
            rows = [r for r in self.hist if r[0] > since]
            blended = [[sp] + list(gains_for(self.table, sp)) for sp in SHOW_SP] if self.table else []
            return {'link': self.state['link'], 'idn': self.state['idn'], 'pid': self.state['pid'],
                    'board_sched': self.state.get('board_sched'), 'pid_set': self.state.get('pid_set'),
                    'log': self.state['log'], 'rows': rows, 'results': self.results, 'vresults': self.vresults,
                    'prog': self.prog_view(), 'last_prog': self.last_prog,
                    'table': self.table_rows, 'table_name': self.table_name, 'blended': blended,
                    'schedule': self.schedule, 'logger': self.logger.view(), 'ls': ls, 'autotune': self.at}

    def csv_text(self):
        f = io.StringIO()
        w = csv.writer(f)
        w.writerow(self.cols())
        with self.lock:
            w.writerows(self.hist)
        return f.getvalue()


def make_handler(board):
    page = open(os.path.join(HERE, 'pmc_gui.html'), 'rb').read()

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def out(self, body, kind='application/json', extra=None):
            if isinstance(body, str):
                body = body.encode()
            self.send_response(200)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == '/':
                self.out(page, 'text/html; charset=utf-8')
            elif self.path.startswith('/api/state'):
                since = float(self.path.partition('since=')[2] or -1)
                self.out(json.dumps(board.snapshot(since)))
            elif self.path == '/api/csv':
                self.out(board.csv_text(), 'text/csv',
                         {'Content-Disposition': 'attachment; filename="pmc_session_%s.csv"' % stamp()})
            elif self.path == '/api/table.csv':
                self.out(board.table_csv(), 'text/csv',
                         {'Content-Disposition': 'attachment; filename="gains_%s.csv"' % stamp()})
            else:
                self.send_error(404)

        def do_POST(self):
            n = int(self.headers.get('Content-Length') or 0)
            out = {}
            try:
                q = json.loads(self.rfile.read(n) or b'{}')
                p = self.path
                if p == '/api/cmd':
                    r = board.send(str(q['line']).upper()[:90])
                elif p == '/api/stop':
                    r = board.stop_all()
                elif p == '/api/raw':
                    line = str(q['line']).strip()[:96]
                    kind = 'events' if line.upper().startswith('EVNT?') else 'raw'
                    got = board.queued(kind, line)
                    out['lines'] = got if isinstance(got, list) else [got]
                    r = ''
                elif p == '/api/events':
                    got = board.queued('events', 'EVNT? %d' % int(q.get('n', 30)))
                    out['lines'] = got if isinstance(got, list) else [got]
                    r = ''
                elif p == '/api/time':
                    r = board.send('TIME SET ' + time.strftime('%Y-%m-%d %H:%M:%S'))
                elif p == '/api/test':
                    sps = q.get('sps')
                    sps = ([float(x) for x in str(sps).replace(',', ' ').split()] if sps is not None
                           else [float(q['start']), float(q['to'])])
                    r = board.start_test(sps, bool(q.get('back')), float(q.get('record', 300)),
                                         float(q.get('hold', SETTLED_S)), float(q.get('band', 0.1)))
                elif p == '/api/vtest':
                    r = board.start_vtest(float(q['at']), float(q['step']), float(q.get('hold', 180)),
                                          float(q.get('record', 600)))
                elif p == '/api/autotune':
                    r = board.start_autotune([float(x) for x in str(q['sps']).replace(',', ' ').split()],
                                             float(q.get('d', 0.4)), float(q.get('div', 4)), float(q.get('b1', 6)),
                                             float(q.get('b2', 20)), int(float(q.get('h', 15))),
                                             float(q.get('kp0', 0.10)), float(q.get('ti0', 0.40)))
                elif p == '/api/autotune/write':
                    r = board.write_sets()
                elif p == '/api/tune':
                    r = board.start_tune([float(x) for x in str(q['sps']).replace(',', ' ').split()],
                                         float(q.get('step', 0.5)), float(q.get('tc', 1.0)))
                elif p == '/api/table':
                    board.load_csv(str(q['csv']), str(q.get('name', '')))
                    r = ''
                elif p == '/api/schedule':
                    if q.get('on') and not board.table:
                        r = 'make or load a gain table first'
                    else:
                        board.schedule, board.sched_sp = bool(q.get('on')), None
                        board.note('gain schedule %s' % ('on' if board.schedule else 'off'))
                        r = ''
                elif p == '/api/log':
                    board.logger.set(on=q.get('on'), new=bool(q.get('new')))
                    board.note('logger: %s' % ('new file' if q.get('new') else 'on' if q.get('on') else 'off'))
                    r = ''
                elif p == '/api/ls':
                    try:
                        r = board.ls.point(str(q.get('host', '')), ls_inputs(str(q.get('inputs', ''))))
                    except ValueError as e:
                        r = str(e)
                elif p == '/api/test/stop':
                    board.stop_gently()
                    r = ''
                else:
                    self.send_error(404)
                    return
            except (ValueError, KeyError, TypeError) as e:
                r = 'bad request: %s' % e
            if isinstance(r, str) and r.startswith('LINK '):
                r = 'no link to the board: ' + r[5:]
            out['reply'] = r
            self.out(json.dumps(out))

    return H


def keep_awake():
    """Ask Windows not to sleep on its own while the GUI runs, so a long
    log has no gaps (the CSV of 6 Nov had none from 17:27 to 10:40: the
    laptop slept). The screen can still turn off. Closing the lid or Sleep
    from the Start menu still sleeps. No admin rights needed; lasts until
    the GUI closes."""
    if sys.platform != 'win32':
        return False
    try:
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        return bool(ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED))
    except (OSError, AttributeError):
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('host', help='board IP, or IP:port (port 8888 by default)')
    ap.add_argument('--ls336', default='', metavar='IP', help='Lake Shore 336 IP, or IP:port (port 7777 by default)')
    ap.add_argument('--ls-inputs', nargs='+', default=['A=Heat exchanger', 'D=Cold head'], metavar='IN[=NAME]',
                    help='336 inputs to read, each with its name (default "A=Heat exchanger" "D=Cold head")')
    ap.add_argument('--log-dir', default='pmc-logs', help='folder for the logs and test results (default pmc-logs)')
    ap.add_argument('--port', type=int, default=8080, help='web page port on this PC (default 8080)')
    ap.add_argument('--bind', default='127.0.0.1',
                    help='address the page listens on (default this PC only; 0.0.0.0 lets other PCs open it)')
    ap.add_argument('--allow-sleep', action='store_true',
                    help='let Windows sleep while the GUI runs (by default it keeps the PC awake)')
    ap.add_argument('--speed', type=float, default=1.0, help=argparse.SUPPRESS)   # simulator only
    a = ap.parse_args()
    try:
        inputs = ls_inputs(a.ls_inputs)
    except ValueError as e:
        sys.exit(str(e))
    board = Board(a.host, a.speed, a.log_dir)
    board.ls = LS336(board)
    board.ls.inputs = inputs              # shown in the page's 336 box before Connect
    if a.ls336:
        board.ls.point(a.ls336, inputs)
    # Windows reserves blocks of ports (Hyper-V, WSL, Docker) and refuses
    # them with WinError 10013 even without admin limits, and 8080 is often
    # in one. If the page port is refused or busy, try the next few, then
    # ports well away from it, then any free port.
    srv = None
    for port in [a.port] + list(range(a.port + 1, a.port + 10)) + [18080, 28080, 0]:
        try:
            srv = ThreadingHTTPServer((a.bind, port), make_handler(board))
            break
        except OSError as e:
            print('page port %d not available (%s), trying another' % (port, e.strerror or e))
    if srv is None:
        sys.exit('no port for the page; try --port with another number')
    port = srv.server_address[1]
    threading.Thread(target=board.run, daemon=True).start()
    print('board %s:%d, open http://localhost:%d  (Ctrl+C to stop)' % (board.addr + (port,)))
    print('logging to %s' % board.logger.folder)
    if not a.allow_sleep and keep_awake():
        print('keeping this PC awake while the GUI runs (the screen can turn off; keep it plugged in, lid open)')
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    board.logger.set(on=False)


if __name__ == '__main__':
    sys.exit(main())
