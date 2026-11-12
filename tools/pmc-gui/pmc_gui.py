#!/usr/bin/env python3
"""Control test GUI for the pressure controller, in a web browser.

    python pmc_gui.py 192.168.1.100                    # then open http://localhost:8080
    python pmc_gui.py 192.168.1.100 --ls336 192.168.1.165

A small web server on this PC talks to the board's text port (TCP 8888), and
to a Lake Shore 336 (TCP 7777) when one is given, and serves one page:
  Live      pressure, setpoint, valve and the 336 temperatures with charts;
            setpoint, AUTO / MANUAL, Go to %, STOP; the PID gains (PID?,
            PID SET); the logger; the 336 connection.
  Gains     the autotune for the board's PID table (pmc_autotune.py: a relay
            test per setpoint for Ku and Pu, first gains from a fit of the
            cryostat's response, a checking step that refines KP and TI, the
            four sets from firmware 0.16.14, Write to board).
  Board     the event log (EVENT?), reset and OK for alarms, the board clock
            from this PC, and a command line for any text port command.
A bar under the tabs shows the autotune while it runs: its steps, the stage it
is in, how long it has taken and about how long is left.

Everything is saved as it happens, in pmc-logs in the folder the GUI is
started from (--log-dir to change it):
  pmc_<date>_<time>.csv    every sample, one a second, with the 336
                           temperatures and the test stage (the logger);
  autotune_<date>_<time>/  one folder per autotune: its samples, its
                           results after every setpoint, its messages and
                           the working. A run that stops, stalls or loses
                           the link keeps everything up to there.
On Windows the GUI keeps the PC from sleeping on its own while it runs
(--allow-sleep to let it), and notes any gap in the samples.

Python 3.8 or later, standard library only: nothing to install, and no
admin rights needed. Keep tools/pmc-gui and tools/gain-schedule side by side
(the autotune and the text-port link are in pmc_autotune.py). The board takes
one text-port connection at a time, so close PuTTY and the logger first. Set
commands need Remote control On.
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
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from types import SimpleNamespace

HERE = os.path.dirname(os.path.abspath(__file__))
for d in (os.path.join(HERE, '..', 'gain-schedule'), HERE):
    if os.path.exists(os.path.join(d, 'pmc_autotune.py')):
        sys.path.insert(0, d)
        break
import pmc_autotune   # noqa: E402
from pmc_autotune import Refused   # noqa: E402

BOARD_PORT = 8888
LS_PORT = 7777
HISTORY = 4 * 3600          # samples kept for the page (1 a second): 4 hours
AUTO_GUESS_S = 1500         # an autotune setpoint, until one has finished
STEP_GUESS_S = 300          # a setpoint step, until one has finished
RIDE_S = 120                # an autotune run waits this long for the link
GAP_S = 60                  # no sample for this long: noted in the event log
COLS = ['t_s', 'sp_mbar', 'p_mbar', 'valve_pct', 'mode', 'alarm', 'stage']
LOG_SHOWN = 200             # the autotune log rows sent to the page (the file has them all)
PREDS_KEPT = 8              # the check steps' predicted lines kept for the charts
# The 336 inputs on this rig, used when an input is given without a name.
LS_NAMES = {'A': 'Heat exchanger', 'D': 'Cold head'}
LS_INPUT = re.compile(r'^[A-D][1-5]?$')
OLD_FIRMWARE = (0, 16, 9)   # the reversal hold scales with KP from this version
NAMES_FIRMWARE = (0, 16, 12)   # the text port names are whole words from this version (PRESSURE?, not PRES?)
TABLE_FIRMWARE = (0, 16, 14)   # four PID sets and SCHED SET with B3 from this version
NOKICK_FIRMWARE = (0, 16, 15)   # P on the pressure only from this version (no kick on a setpoint step)
GUI_BUILD = ''              # set by main(): gui_build()


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


def gui_build(here=HERE):
    """This GUI's build, from the git checkout it runs in: 'build <commits> (<hash>), <commit time>',
    and ', edited' when its files differ from that commit. Without git: the newest file time."""
    import subprocess

    def git(*a):
        r = subprocess.run(['git', '-C', here] + list(a), capture_output=True, text=True, timeout=5)
        if r.returncode:
            raise OSError(r.stderr.strip())
        return r.stdout.strip()
    try:
        n = int(git('rev-list', '--count', 'HEAD'))
        h, ct = git('log', '-1', '--format=%h %ct').split()
        try:
            edited = git('status', '--porcelain', '--untracked-files=no', '--', '.', os.path.join('..', 'gain-schedule'))
        except (OSError, subprocess.SubprocessError):
            edited = ''
        return 'build %d (%s), %s%s' % (n, h, time.strftime('%d/%m/%y %H:%M', time.localtime(int(ct))),
                                        ', edited' if edited else '')
    except (OSError, ValueError, subprocess.SubprocessError):
        files = [os.path.join(here, f) for f in os.listdir(here) if f.endswith(('.py', '.html'))]
        t = max((os.path.getmtime(f) for f in files), default=0)
        return 'files of %s (no git)' % time.strftime('%d/%m/%y %H:%M', time.localtime(t))


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
    """pmc_autotune.Link's ask/do, through the GUI's one connection."""

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


class GuiAutoTuner(pmc_autotune.AutoTuner):
    """pmc_autotune.AutoTuner that tells the page which stage it is in."""

    def say(self, msg):
        self.board.note('autotune: ' + msg)

    def stage(self, text, phase=None, most=None):
        if phase:
            self.phase_now = phase
        self.board.stage(text, phase, most)

    def show(self):
        with self.board.lock:
            if self.board.at is not None:
                self.board.at['calc'], self.board.at['calc_now'] = list(self.calcs), self.calc_now
                self.board.at['log'] = self.log_rows[-LOG_SHOWN:]

    def predicted(self, rows, label):
        # The check step's predicted answer, for the charts: drawn from the
        # board time the step starts, ahead of the readings.
        b = self.board
        with b.lock:
            self.pred_open = {'t0': b.now(), 'label': label,
                              'pts': [[t, round(p, 3), round(x, 2)] for t, p, x in rows]}
            b.preds = (b.preds + [self.pred_open])[-PREDS_KEPT:]
            b.preds_v += 1

    def predicted_end(self, t):
        # The step ended (settled sooner, the watchdog, or AUTO left): its
        # line stops there.
        b, q = self.board, getattr(self, 'pred_open', None)
        if q is None:
            return
        with b.lock:
            q['pts'] = [r for r in q['pts'] if r[0] <= t] or q['pts'][:1]
            b.preds_v += 1
        self.pred_open = None

    def log(self, kind, text, change=''):
        # The calculations, decisions, gain changes and commands go to
        # autotune_log.csv in the run folder (pmc_autotune.AutoTuner.log);
        # the page shows all but the calculations, which the Working card has.
        super().log(kind, text, change)
        if kind != 'calc':
            self.show()

    def left(self, want, mode):
        alarm = self.board.hist[-1][5] if self.board.hist else ''
        return 'the board left %s (now %s%s)' % (want, mode or 'no mode',
                                                  ', ' + alarm if alarm and alarm != 'NONE' else '')


class GuiStepTest(pmc_autotune.StepTest):
    """pmc_autotune.StepTest that shows its steps on the run bar and its
    figures on the Gains tab."""

    def say(self, msg):
        self.board.note('step test: ' + msg)

    def step_now(self, i, label, most):
        b = self.board
        b.step(i, 'now')
        b.stage(label, 'step test: ' + b.prog['steps'][i]['label'] if b.prog else 'step test', most)

    def step_done(self, i, note):
        self.board.step(i, 'done', note)
        self.say('%s: %s' % (self.board.prog['steps'][i]['label'] if self.board.prog else 'step %d' % i, note))

    def result(self, r):
        b = self.board
        b.result('results.csv', r)
        with b.lock:
            if b.st is not None:
                b.st['rows'] = b.st['rows'] + [r]

    def now(self, text):
        with self.board.lock:
            if self.board.st is not None:
                self.board.st['now'] = text


class Board:
    """One connection, owned by one thread: it polls once a second and runs
    the lines the page and the tuner hand it through a queue in between."""

    def __init__(self, host, speed=1.0, log_dir='pmc-logs'):
        h, _, p = host.partition(':')
        self.addr = (h, int(p or BOARD_PORT))
        self.speed = speed
        self.sock, self.buf = None, b''
        self.lock = threading.Lock()
        self.llock = threading.Lock()      # the learned file: page refreshes and the autotune
        self.cmds = queue.Queue()
        self.hist = []            # COLS + one per 336 input
        self.t0 = time.time()
        self.last_wall = None     # PC time of the last sample, to see a gap (the PC asleep)
        self.state = {'link': 'connecting', 'idn': '', 'pid': None, 'log': []}
        self.logger = Logger(log_dir)
        self.ls = None
        self.job = None           # 'autotune' while it runs
        self.stop_job = self.abort = False
        self.link_wait = None
        self.prog, self.last_prog = None, None    # what the run bar shows
        self.phase = ''           # the stage column of every sample
        self.run_dir, self.run_files, self.run_log = None, {}, None
        self.at = None            # the last autotune: its rows, the sets, written or not
        self.st = None            # the last setpoint step test: its steps' figures
        self.preds, self.preds_v = [], 0   # predicted lines (GuiAutoTuner.predicted), sent when they change

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
        fw = r.split(',')[3:4]
        ver = tuple(int(x) for x in re.findall(r'\d+', fw[0])[:3]) if fw else None
        if ver and ver < NAMES_FIRMWARE:
            raise ConnectionError('board firmware %s has the old command names: flash %s or later' % (
                fw[0].strip(), '.'.join(map(str, NAMES_FIRMWARE))))
        self.fw_ver = ver
        # The autotune's predicted line and its gains follow the board's PI.
        pmc_autotune.PI_KICK = bool(ver) and ver < NOKICK_FIRMWARE
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
            if not first.startswith('EVENT '):
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
        r = self.ask('SETPOINT?;PRESSURE?;VALVE?;MODE?;ALARM?;PID?;SCHED?;PIDS?')
        sp, p, v, m, a, pid, sch, pids = (r.split(';') + [''] * 8)[:8]
        row = [self.now(), num(sp), num(p), num(v), m[5:], a[6:], self.phase] + (self.ls.now() if self.ls else [])
        self.add_row(row)
        if pid.startswith('PID '):
            with self.lock:
                self.state['pid'] = [float(x) for x in pid[4:].split(',')]
        # The board's own gain schedule (0.15.15): which set PID? is, and on what.
        with self.lock:
            self.state['board_sched'] = sch[6:].split(',')[0] if sch.startswith('SCHED ') else None
            self.state['pid_set'] = int(pids[5:].split(',')[0]) if pids.startswith('PIDS ') else None

    def busy(self):
        return self.job

    # A test's run: its folder, the run bar ----------------------------------
    def begin(self, kind, name, labels, settings):
        """A test starts: its folder (samples, messages), and the run bar."""
        t = self.now()
        d = os.path.join(self.logger.folder, '%s_%s' % (kind, stamp()))
        with self.lock:
            self.prog = {'kind': kind, 'name': name, 't0': t, 'i': None,
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
        # About how long is left: what each finished setpoint took, or a guess until one has.
        steps_test = P['kind'] == 'steptest'
        pts = P['steps'][1:] if steps_test else P['steps'][:-1]   # not the step test's first hold, the autotune's sets
        took = [x['t1'] - x['t0'] for x in pts if x['state'] == 'done' and x['t0'] is not None]
        per = sum(took) / len(took) if took else STEP_GUESS_S if steps_test else AUTO_GUESS_S
        todo = sum(1 for x in pts if x['state'] == 'todo')
        left, how = max(per - in_step, 60) + per * todo, 'about'
        wait = None
        if self.link_wait is not None:
            wait = 'waiting for the board link: %d of %d s' % ((time.monotonic() - self.link_wait) * self.speed, RIDE_S)
        steps = self.steps_view(P['steps'])
        return {'kind': P['kind'], 'name': P['name'], 'i': i, 'steps': steps, 'stage': P['stage'],
                'stage_s': t - P['stage_t'], 'stage_max': P['stage_max'], 'elapsed_s': t - P['t0'],
                'left_s': left, 'how': how, 'wait': wait, 'dir': P['dir'],
                'done_at': time.strftime('%H:%M', time.localtime(time.time() + left / self.speed))}

    # Autotune (pmc_autotune.py) ------------------------------------------
    def learned_path(self):
        return os.path.join(self.logger.folder, pmc_autotune.LEARNED_FILE)

    def learned_load(self):
        """The learned gains. With no learned file yet, built from the earlier
        autotune runs' results.csv in the log folder (and saved)."""
        path = self.learned_path()
        with self.llock:
            have = os.path.exists(path)
            learned = pmc_autotune.load_learned(path) if have else pmc_autotune.seed_learned(self.logger.folder)
            if learned and not have:
                try:
                    pmc_autotune.save_learned(path, learned)
                except OSError:
                    pass
        return learned

    def learned_list(self):
        """[(setpoint, kp, ti, when)] from the learned file, for the card."""
        L = self.learned_load()
        return [[L[k]['sp_mbar'], L[k]['kp'], L[k]['ti_min'], L[k].get('when') or ''] for k in sorted(L, key=float)]

    def table_old(self):
        """Why the board cannot take a four-set PID table, or ''."""
        v = getattr(self, 'fw_ver', None)
        if v and v < TABLE_FIRMWARE:
            return 'the PID table with B3 (four sets) needs firmware %s or later: flash it first' % (
                '.'.join(map(str, TABLE_FIRMWARE)))
        return ''

    def start_autotune(self, sps, d, b1, b2, b3, h, kp0=0.10, ti0=0.40, start='learned'):
        if self.busy():
            return 'a test is already running'
        # In the order typed (Ryan 10 Nov asked about going down in mbar:
        # "24 16 12 8 4 2" runs high to low); a repeat runs once.
        sps = list(dict.fromkeys(round(x, 1) for x in sps if x > 0))
        if not sps:
            return 'give at least one setpoint'
        if not 0 < b1 < b2:
            return 'B1 must be above 0 and below B2'
        if not (b3 == 0 or b3 > b2):
            return 'B3 must be above B2, or 0 for three sets'
        if not 0 <= h <= 50:
            return 'H must be 0 to 50 %'
        if not 0.1 <= d <= pmc_autotune.D_MAX:
            return 'the relay step must be 0.1 to %g %%' % pmc_autotune.D_MAX
        if not (pmc_autotune.KP_MIN <= kp0 <= pmc_autotune.KP_MAX and pmc_autotune.TI_MIN <= ti0 <= pmc_autotune.TI_MAX):
            return 'the first gains must be KP %g to %g and TI %g to %g min' % (
                pmc_autotune.KP_MIN, pmc_autotune.KP_MAX, pmc_autotune.TI_MIN, pmc_autotune.TI_MAX)
        r = self.raw('SETPOINT?;PRESSURE?;VALVE?;ALARM?;PID?')
        if r.startswith('LINK '):
            return r
        parts = (r.split(';') + [''] * 5)[:5]
        if parts[1] == 'PRESSURE NONE':
            return 'no pressure reading'
        if parts[2] == 'VALVE NONE':
            return 'the valve is not homed: home it on the screen first'
        if 'F-' in parts[3]:
            return 'reset the fault first (Board tab): ' + parts[3][6:]
        if not parts[4].startswith('PID '):
            return 'this firmware has no PID SET (0.15.8 or later): ' + parts[4]
        r = self.send('PID SET ' + parts[4][4:])   # the same gains, so nothing is written: ERR 211 when Remote control is Off
        if r:
            return r
        a = SimpleNamespace(sps=sps, d=d, b1=b1, b2=b2, b3=b3, h=h, kp0=kp0, ti0=ti0, learned=start != 'fresh')
        self.job, self.stop_job, self.abort = 'autotune', False, False
        self.begin('autotune', 'PID autotune', ['%g mbar' % x for x in sps] + ['PID table sets'],
                   'setpoints %s mbar, first gains KP %.2f TI %.2f, first relay step %.1f %%, '
                   'PID table B1 %g B2 %g B3 %g H %d %%, start from %s' % (
                       ' '.join('%g' % x for x in sps), kp0, ti0, d, b1, b2, b3, h, 'learned gains' if a.learned else 'fresh'))
        with self.lock:
            self.preds, self.preds_v = [], self.preds_v + 1
            self.at = {'rows': [], 'sets': [], 'b1': b1, 'b2': b2, 'b3': b3, 'h': h, 'dir': self.run_dir, 'lines': [],
                       'written': '', 'running': True, 'calc': [], 'calc_now': '', 'sets_calc': []}
        threading.Thread(target=self.autotune_job, args=(a,), daemon=True).start()
        return ''

    # Setpoint step test (pmc_autotune.StepTest) ------------------------------
    def start_steptest(self, sps, most_min, table=None, hold_mbar=pmc_autotune.STEP_HOLD_MBAR,
                       hold_min=pmc_autotune.STEP_HOLD_S / 60):
        """AUTO through the setpoints in the order typed, each step until it
        holds (the 10 s mean within hold_mbar of the setpoint for hold_min,
        Ryan 12 Nov) or most_min, with the board's gains and PID table
        (Ryan, 11 Nov). table:
        {'sets': [[kp, ti, kd]] x 4, 'b1', 'b2', 'b3', 'h'} (four sets from
        0.16.14; b3 0: the first three) is written to the board first; None
        leaves the board's as they are."""
        if self.busy():
            return 'a test is already running'
        sps = [round(x, 1) for x in sps]
        if len(sps) < 2 or any(x <= 0 for x in sps) or any(a == b for a, b in zip(sps, sps[1:])):
            return 'give two or more setpoints above 0, each different from the one before'
        if not 1 <= most_min <= 120:
            return 'the most per step must be 1 to 120 min'
        if not 0.01 <= hold_mbar <= 5:
            return 'hold within 0.01 to 5 mbar'
        if not 0.5 <= hold_min < most_min:
            return 'the hold must be 0.5 min or more, and shorter than the most per step'
        lines = []
        if table:
            sets = [[float(v) for v in x] for x in table['sets']]
            b1, b2, h = float(table['b1']), float(table['b2']), int(float(table['h']))
            b3 = float(table.get('b3', 0))
            if len(sets) != 4 or any(len(x) != 3 for x in sets):
                return 'give KP, TI and KD for each of the four sets'
            if b3 == 0:
                sets = sets[:3]        # three sets: set 4 is not used
            for n, (kp, ti, kd) in enumerate(sets, 1):
                if not (pmc_autotune.KP_MIN <= kp <= pmc_autotune.KP_MAX and pmc_autotune.TI_MIN <= ti <= pmc_autotune.TI_MAX
                        and 0 <= kd <= 10000):
                    return 'set %d: KP %g to %g, TI %g to %g min, KD 0 or more' % (
                        n, pmc_autotune.KP_MIN, pmc_autotune.KP_MAX, pmc_autotune.TI_MIN, pmc_autotune.TI_MAX)
            if not 0 < b1 < b2:
                return 'B1 must be above 0 and below B2'
            if not (b3 == 0 or b3 > b2):
                return 'B3 must be above B2, or 0 for three sets'
            if not 0 <= h <= 50:
                return 'H must be 0 to 50 %'
            lines = pmc_autotune.table_lines(sets, b1, b2, b3, h)
            r = self.table_old()
            if r:
                return r + ", or pick Leave the board's"
        r = self.raw('SETPOINT?;PRESSURE?;VALVE?;ALARM?')
        if r.startswith('LINK '):
            return r
        parts = (r.split(';') + [''] * 4)[:4]
        if parts[1] == 'PRESSURE NONE':
            return 'no pressure reading'
        if parts[2] == 'VALVE NONE':
            return 'the valve is not homed: home it on the screen first'
        if 'F-' in parts[3]:
            return 'reset the fault first (Board tab): ' + parts[3][6:]
        r = self.send('SETPOINT %.1f' % sps[0])     # ERR 211 when Remote control is Off
        if r:
            return r
        for line in lines:
            r = self.send(line)
            if r.startswith('ERR 100'):
                return 'this firmware has no PID table (0.15.15 or later)'
            if r:
                return '%s: %s' % (line, r)
        most, hold_s = int(most_min * 60), int(round(hold_min * 60))
        self.job, self.stop_job, self.abort = 'steptest', False, False
        labels = ['Hold %g mbar' % sps[0]] + ['%g -> %g mbar' % (a, b) for a, b in zip(sps, sps[1:])]
        self.begin('steptest', 'Setpoint step test', labels,
                   '%s mbar, each step until the %d s mean holds within %g mbar of the setpoint for %g min, '
                   'or %g min at most; %s' % (
                       ' -> '.join('%g' % x for x in sps), pmc_autotune.STEP_MEAN_S, hold_mbar, hold_s / 60.0,
                       most / 60.0,
                       'PID table written first: ' + '; '.join(lines) if lines else 'the board\'s own gains and PID table'))
        with self.lock:
            self.st = {'sps': sps, 'rows': [], 'now': '', 'running': True, 'dir': self.run_dir, 'written': lines}
        threading.Thread(target=self.steptest_job, args=(sps, most, hold_mbar, hold_s), daemon=True).start()
        return ''

    def steptest_job(self, sps, most, hold_mbar=pmc_autotune.STEP_HOLD_MBAR, hold_s=pmc_autotune.STEP_HOLD_S):
        t = GuiStepTest(ViaBoard(self), pmc_autotune.Clock(self.speed), None, self.run_dir)
        t.board = self
        outcome = 'done'
        try:
            t.run(sps, most, hold_mbar, hold_s)
        except Stopped:
            outcome = 'ended by STOP' if self.abort else 'stopped'
        except (RuntimeError, Refused, ConnectionError, ValueError) as e:
            outcome = 'ended: %s' % e
        finally:
            self.stop_job = False
            if outcome != 'done' and self.prog:
                with self.lock:
                    self.where(self.prog)
            # Stop test puts the setpoint back to the first one; STOP leaves everything.
            if outcome == 'stopped':
                r = self.send('SETPOINT %.1f' % sps[0])
                outcome += '; setpoint back to %g mbar%s' % (sps[0], ' (' + r + ')' if r else '')
            with self.lock:
                if self.st is not None:
                    self.st['running'], self.st['now'] = False, ''
            self.end(outcome)
            self.job = None

    def at_rows(self, rows, a):
        """The results so far: the sets, and the files that hold them."""
        sets = pmc_autotune.table_sets(rows, a.b1, a.b2, a.b3)
        lines = pmc_autotune.write_lines(sets, a.b1, a.b2, a.b3, a.h)
        how = []
        for n, kp, ti, pts in sets:
            if kp is None:
                continue
            rs = [r for r in rows if r['sp_mbar'] in pts]
            if len(rs) == 1:
                how.append('Set %d: KP %.2f and TI %.2f min, from %g mbar alone' % (n, kp, ti, pts[0]))
            else:
                how.append('Set %d: KP = the lowest of %s = %.2f; TI = geometric mean of %s = %.2f min' % (
                    n, ', '.join('%.2f' % r['kp'] for r in rs), kp, ', '.join('%.2f' % r['ti_min'] for r in rs), ti))
        with self.lock:
            self.at.update(rows=list(rows), sets=[list(x) for x in sets], lines=lines, sets_calc=how)
        if not self.run_dir:
            return
        try:
            with open(os.path.join(self.run_dir, 'sets.csv'), 'w', newline='', encoding='utf-8') as f:
                w = csv.writer(f)
                w.writerow(['set', 'kp', 'ti_min', 'from_mbar', 'b1_mbar', 'b2_mbar', 'b3_mbar', 'h_pct'])
                for n, kp, ti, pts in sets:
                    w.writerow([n, kp, ti, ' '.join('%g' % x for x in pts), a.b1, a.b2, a.b3, a.h])
            with open(os.path.join(self.run_dir, 'write-to-board.txt'), 'w', encoding='utf-8') as f:
                f.write('\n'.join(lines) + '\n')
        except OSError as e:
            self.note('could not save the autotune results: %s' % e)

    def autotune_job(self, a):
        link = ViaBoard(self)
        t = GuiAutoTuner(link, pmc_autotune.Clock(self.speed), a, self.run_dir)
        t.board = self
        f, rows, outcome, skipped = None, [], 'done', []
        last = len(a.sps)
        try:
            f = pmc_autotune.found(link)
            t.say('found setpoint %s mbar, %s, PID %s, PID table %s' %
                  (f['sp'], f['mode'], ', '.join('%.2f' % x for x in f['pid']), f['sched'] or 'not in this firmware'))
            t.gains = (list(f['pid']) + [0.0] * 3)[:3]
            t.log('start', 'PID autotune: setpoints %s mbar, first gains KP %.2f TI %.2f, first relay step %.1f %%, '
                  'PID table B1 %g B2 %g H %d %%, start from %s. Found: setpoint %s mbar, %s, PID %s, PID table %s; '
                  'firmware %s; GUI %s' % (
                      ' '.join('%g' % x for x in a.sps), a.kp0, a.ti0, a.d, a.b1, a.b2, a.h,
                      'learned gains' if a.learned else 'fresh', f['sp'], f['mode'],
                      ', '.join('%.2f' % x for x in f['pid']), f['sched'] or 'not in this firmware',
                      (self.state.get('idn') or '').split(',')[-1].strip() or '--', GUI_BUILD or '--'))
            t.n_sets = len(f.get('sets') or [])
            if t.n_sets:
                t.log('decision', 'The gains under test go in all %d PID sets (PIDS SET 1 to %d), so the set the PID '
                      'table has in use never matters; the %d sets are put back at the end' % ((t.n_sets,) * 3))
            fw = (self.state.get('idn') or '').split(',')[3:4]
            ver = tuple(int(x) for x in re.findall(r'\d+', fw[0])[:3]) if fw else None
            if ver and ver < OLD_FIRMWARE:
                t.calc('Firmware %s: before %s the reversal hold is a fixed 0.10 %%, which on small steps lets the '
                       'pressure sag under the setpoint and read as overshoot, so KP may be cut more than it needs. '
                       'This autotune is meant for %s or later, where the hold scales with KP.' % (
                           fw[0].strip(), '.'.join(map(str, OLD_FIRMWARE)), '.'.join(map(str, OLD_FIRMWARE))))
                t.say('firmware %s is older than %s: the fixed reversal hold can make the pressure sag and KP be cut '
                      'more than it needs' % (fw[0].strip(), '.'.join(map(str, OLD_FIRMWARE))))
            gains = (a.kp0, a.ti0)      # to reach the first point; each later one uses the point before's
            ku = None
            # Learned gains (Ryan, 9 Nov: refine, not start fresh): a point
            # with a learned row reaches with its own gains, sizes the relay
            # from its own Ku and starts the check from its own KP and TI.
            path = self.learned_path()
            learned = self.learned_load()
            use = learned if a.learned else {}
            t.say('%s: %s' % ('learned gains' if a.learned else 'fresh (learned gains not used)',
                              ', '.join('%s mbar KP %.2f TI %.2f' % (k, v['kp'], v['ti_min'])
                                        for k, v in sorted(learned.items(), key=lambda kv: float(kv[0])))
                              or 'none yet'))
            for i, sp in enumerate(a.sps):
                self.step(i, 'now')
                prior = use.get(pmc_autotune.learned_key(sp))
                try:
                    r = t.point(sp, (prior['kp'], prior['ti_min']) if prior else gains,
                                pmc_autotune.next_d(sp, prior.get('Ku') if prior and prior.get('Ku') else ku, a.d),
                                prior)
                except RuntimeError as e:
                    # One point with no usable oscillation (or a warning that
                    # dropped AUTO) should not lose the others.
                    t.say('%g mbar skipped: %s' % (sp, e))
                    t.log('decision', '%g mbar skipped: %s' % (sp, e))
                    self.step(i, 'failed', str(e))
                    skipped.append('%g' % sp)
                    continue
                rows.append(r)
                self.result('results.csv', r)
                pmc_autotune.learn(learned, r, time.strftime('%d/%m/%y %H:%M'))
                try:
                    with self.llock:
                        pmc_autotune.save_learned(path, learned)
                except OSError as e:
                    self.note('could not save the learned gains: %s' % e)
                self.step(i, 'done', 'Ku %.2f, KP %.2f, TI %.2f%s' % (r['Ku'], r['kp'], r['ti_min'],
                                                                     ', trimmed ' + r['trims'] if r['trims'] else ''))
                self.at_rows(rows, a)
                ku, gains = r['Ku'], (r['kp'], r['ti_min'])
            self.step(last, 'now')
            if rows:
                sets = pmc_autotune.table_sets(rows, a.b1, a.b2, a.b3)
                self.step(last, 'done', '; '.join('set %d %s' % (n, 'KP %.2f TI %.2f' % (kp, ti) if kp is not None
                                                                   else 'not measured') for n, kp, ti, _ in sets))
                t.sp_now, t.phase_now = None, 'PID table sets'
                with self.lock:
                    how = list(self.at.get('sets_calc') or [])
                t.log('result', 'PID table sets (not written to the board; Write to board sends them): ' +
                      '; '.join(how or ['none']))
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
            t.sp_now, t.phase_now = None, 'put back'
            if self.abort:
                t.say('STOP pressed: nothing put back')
                t.log('decision', 'STOP pressed: nothing put back')
            elif f:
                back = pmc_autotune.put_back_lines(f)
                self.stage('putting back what it found: ' + ', '.join(back), 'put back')
                for c in back:
                    r = self.send(c)
                    t.say('put back: %s%s' % (c, ' (' + r + ')' if r else ''))
                    t.log('command', 'put back as found: %s%s' % (c, ' (' + r + ')' if r else ''))
            # The gains in force at the end, from the board (with the PID
            # table back on, the set it picks).
            pid = self.raw('PID?')
            if pid.startswith('PID '):
                try:
                    new = [float(x) for x in pid[4:].split(',')][:3]
                    t.set_gains(new, 'PID? at the end', 'the board after the put back')
                except ValueError:
                    pass
            t.log('end', 'PID autotune %s' % outcome)
            self.end(outcome)
            self.job = None

    def write_sets(self):
        """The sets and the PID table (pressure) to the board."""
        if self.busy():
            return 'a test is running'
        with self.lock:
            at = self.at
            lines = list(at['lines']) if at else []
        if not lines or not any(x[1] is not None for x in at['sets']):
            return 'run the autotune first'
        r = self.table_old()
        if r:
            return r
        for line in lines:
            r = self.send(line)
            if r.startswith('ERR 100'):
                return 'this firmware has no PID table (0.15.15 or later)'
            if r:
                return '%s: %s' % (line, r)
        with self.lock:
            at['written'] = time.strftime('%H:%M')
        self.note('autotune: PID table written to the board: ' + '; '.join(lines))
        for line in lines:     # in the run's log, after its end
            m = re.match(r'PIDS SET (\d),([\d.]+),([\d.]+),([\d.]+)$', line)
            pmc_autotune.log_row(at.get('dir'), [pc_time(), '', '', 'Write to board', 'gains', '', '', '',
                                                 'set %s: KP %s, TI %s, KD %s' % m.groups() if m else '',
                                                 'Write to board: ' + line])
        return ''

    # STOP ----------------------------------------------------------------
    def stop_all(self):
        """STOP: the valve stops where it is and the autotune ends, with nothing
        put back (Stop test, by contrast, puts back what the autotune found)."""
        self.abort = True
        if self.job:
            self.stop_job = True
        return self.send('STOP')

    def stop_gently(self):
        if self.job:
            self.abort, self.stop_job = False, True

    # For the page --------------------------------------------------------
    def snapshot(self, since, pv=None):
        """The page's state: rows after `since`; the predicted lines only when
        the page's version `pv` is not the current one."""
        ls = self.ls.view() if self.ls else None
        learned = self.learned_list()
        with self.lock:
            rows = [r for r in self.hist if r[0] > since]
            out = {'link': self.state['link'], 'idn': self.state['idn'], 'pid': self.state['pid'], 'build': GUI_BUILD,
                   'board_sched': self.state.get('board_sched'), 'pid_set': self.state.get('pid_set'),
                   'log': self.state['log'], 'rows': rows, 'prog': self.prog_view(), 'last_prog': self.last_prog,
                   'logger': self.logger.view(), 'ls': ls, 'autotune': self.at, 'steptest': self.st,
                   'learned': learned,
                   'preds_v': self.preds_v}
            if pv != self.preds_v:
                out['preds'] = list(self.preds)
            return out

    def autotune_log(self):
        """(file name, text) of the last autotune's log, or None."""
        with self.lock:
            d = self.at.get('dir') if self.at else None
        if not d:
            return None
        try:
            with open(os.path.join(d, pmc_autotune.LOG_FILE), encoding='utf-8') as f:
                return '%s_%s' % (os.path.basename(d), pmc_autotune.LOG_FILE), f.read()
        except OSError:
            return None

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
                q = urllib.parse.parse_qs(urllib.parse.urlsplit(self.path).query)
                since = float((q.get('since') or ['-1'])[0] or -1)
                pv = int((q.get('pv') or ['-1'])[0] or -1)
                self.out(json.dumps(board.snapshot(since, pv)))
            elif self.path == '/api/csv':
                self.out(board.csv_text(), 'text/csv',
                         {'Content-Disposition': 'attachment; filename="pmc_session_%s.csv"' % stamp()})
            elif self.path == '/api/autotune/log':
                got = board.autotune_log()
                if got is None:
                    self.send_error(404, 'no autotune log yet')
                else:
                    self.out(got[1], 'text/csv', {'Content-Disposition': 'attachment; filename="%s"' % got[0]})
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
                    kind = 'events' if line.upper().startswith('EVENT?') else 'raw'
                    got = board.queued(kind, line)
                    out['lines'] = got if isinstance(got, list) else [got]
                    r = ''
                elif p == '/api/events':
                    got = board.queued('events', 'EVENT? %d' % int(q.get('n', 30)))
                    out['lines'] = got if isinstance(got, list) else [got]
                    r = ''
                elif p == '/api/time':
                    r = board.send('TIME SET ' + time.strftime('%Y-%m-%d %H:%M:%S'))
                elif p == '/api/autotune':
                    r = board.start_autotune([float(x) for x in str(q['sps']).replace(',', ' ').split()],
                                             float(q.get('d', 0.4)), float(q.get('b1', pmc_autotune.B1)),
                                             float(q.get('b2', pmc_autotune.B2)), float(q.get('b3', pmc_autotune.B3)),
                                             int(float(q.get('h', pmc_autotune.HYST))),
                                             float(q.get('kp0', 0.10)), float(q.get('ti0', 0.40)),
                                             str(q.get('start', 'learned')))
                elif p == '/api/steptest':
                    r = board.start_steptest([float(x) for x in str(q['sps']).replace(',', ' ').split()],
                                             float(q.get('most_min', pmc_autotune.STEP_MOST_S / 60)),
                                             q.get('table'),
                                             float(q.get('hold_mbar', pmc_autotune.STEP_HOLD_MBAR)),
                                             float(q.get('hold_min', pmc_autotune.STEP_HOLD_S / 60)))
                elif p == '/api/steptest/random':
                    try:
                        out['sps'] = pmc_autotune.random_sps(int(float(q.get('n', pmc_autotune.STEP_RANDOM_N))),
                                                             float(q.get('lo', pmc_autotune.STEP_RANDOM_LO)),
                                                             float(q.get('hi', pmc_autotune.STEP_RANDOM_HI)))
                        r = ''
                    except ValueError as e:
                        r = str(e)
                elif p == '/api/autotune/write':
                    r = board.write_sets()
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
    global GUI_BUILD
    GUI_BUILD = gui_build()
    print('iPC control test GUI %s' % GUI_BUILD)
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
