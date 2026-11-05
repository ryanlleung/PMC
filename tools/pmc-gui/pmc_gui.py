#!/usr/bin/env python3
"""Control test GUI for the pressure controller, in a web browser.

    python pmc_gui.py 192.168.1.100          # then open http://localhost:8080
    python pmc_gui.py 192.168.1.100 --port 8090

A small web server on this PC talks to the board's text port (TCP 8888) and
serves one page with every board tool in it:
  Live      pressure, setpoint and valve with a chart, mode and alarms;
            setpoint, AUTO / MANUAL, Go to %, STOP; the PID gains (PID?,
            PID SET); CSV of every sample (the logger).
  Tests     1. valve step test in MANUAL: the plant's gain, delay and time
            constant, and the KP and TI they suggest; 2. setpoint step test
            in AUTO: overshoot and settling time.
  Gains     the gain table over several setpoints (pmc_gains.py tune), its
            blended KP and TI (show), and the gain schedule (run), which
            sets KP and TI each time the setpoint changes.
  Board     the event log (EVNT?), reset and OK for alarms, the board clock
            from this PC, and a command line for any text port command.

Python 3.8 or later, standard library only: nothing to install, and no
admin rights needed. Keep tools/pmc-gui and tools/gain-schedule side by side
(the step fit and the tuner are in pmc_gains.py). The board takes one
text-port connection at a time, so close PuTTY, the logger and
pmc_gains.py first. Set commands need Remote control On.
"""
import argparse
import csv
import io
import json
import os
import queue
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
from pmc_gains import fit_step, simc, gains_for, load_table, Refused   # noqa: E402

BOARD_PORT = 8888
HISTORY = 4 * 3600          # samples kept (1 a second): 4 hours
SHOW_SP = [1, 1.5, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 25]


def num(part):
    w = part.split()
    try:
        return float(w[-2] if w[-1] in ('mbar', '%') else w[-1])
    except (ValueError, IndexError):
        return None


def metrics(rows, p0, p1, band):
    """rows: (s from the step, pressure). The step response figures."""
    step = p1 - p0
    if abs(step) < 1e-6 or not rows:
        return None
    t90 = next((t for t, p in rows if (p - p0) / step >= 0.9), None)
    peak = max((p - p0) / step for _, p in rows)
    settle = 0.0
    for t, p in rows:
        if abs(p - p1) > band:
            settle = t
    last = [p for t, p in rows if t >= rows[-1][0] - 60]
    return {'from': p0, 'to': p1, 't90_s': None if t90 is None else round(t90),
            'overshoot_pct': round(max(0.0, (peak - 1) * 100), 1),
            'settle_s': round(settle) if settle < rows[-1][0] else None,
            'mean': round(statistics.mean(last), 3),
            'spread': round(statistics.pstdev(last), 3) if len(last) > 1 else 0.0}


class Stopped(Exception):
    """Stop test pressed while the tuner was running."""


class ViaBoard:
    """pmc_gains.Link's ask/do, through the GUI's one connection."""

    def __init__(self, board):
        self.b = board

    def ask(self, line):
        if self.b.stop_job:
            raise Stopped()
        r = self.b.raw(line)
        if r.startswith('LINK '):
            raise ConnectionError(r[5:])
        parts = r.split(';')
        if parts[0].startswith('ERR'):
            raise Refused(parts[0])
        return parts

    def do(self, cmd):
        return self.ask(cmd + ';MODE?')

    def close(self):
        pass


class GuiTuner(pmc_gains.Tuner):
    def say(self, msg):
        self.board.note('gain table: ' + msg)


class Board:
    """One connection, owned by one thread: it polls once a second and runs
    the lines the page and the tuner hand it through a queue in between."""

    def __init__(self, host, speed=1.0):
        h, _, p = host.partition(':')
        self.addr = (h, int(p or BOARD_PORT))
        self.speed = speed
        self.sock, self.buf = None, b''
        self.lock = threading.Lock()
        self.cmds = queue.Queue()
        self.hist = []            # [t, sp, p, valve, mode, alarm, phase]
        self.t0 = time.time()
        self.state = {'link': 'connecting', 'idn': '', 'pid': None, 'log': []}
        self.test = None          # setpoint step test in progress
        self.results = []
        self.vtest = None         # valve step test in progress
        self.vresults = []
        self.job = None           # 'gain table' while the tuner runs
        self.stop_job = False
        self.table, self.table_rows, self.table_name = None, [], ''
        self.schedule, self.sched_sp = False, None

    # Link ----------------------------------------------------------------
    def connect(self):
        self.sock = socket.create_connection(self.addr, timeout=5)
        self.buf = b''
        self.state['idn'] = self.ask('*IDN?')
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
            self.state['log'] = (self.state['log'] + [time.strftime('%H:%M:%S ') + text])[-60:]

    # Loop ----------------------------------------------------------------
    def run(self):
        while True:
            try:
                if self.sock is None:
                    self.connect()
                    self.note('connected: ' + self.state['idn'])
                self.serve()
            except (OSError, ConnectionError) as e:
                self.state['link'] = 'no link: %s' % e
                self.note('link lost: %s' % e)
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

    def poll(self):
        r = self.ask('PRES SP?;PRES?;VALV?;MODE?;ALRM?;PID?')
        sp, p, v, m, a, pid = (r.split(';') + [''] * 6)[:6]
        t = round((time.time() - self.t0) * self.speed, 1)
        phase = (self.test['phase'] if self.test else self.vtest['phase'] if self.vtest else self.job or '')
        row = [t, num(sp), num(p), num(v), m[5:], a[5:], phase]
        with self.lock:
            self.hist.append(row)
            del self.hist[:-HISTORY]
            if pid.startswith('PID '):
                self.state['pid'] = [float(x) for x in pid[4:].split(',')]
        if self.test:
            self.step_tick(row)
        if self.vtest:
            self.vstep_tick(row)
        self.schedule_tick(row)

    def busy(self):
        return self.test or self.vtest or self.job

    # Setpoint step test (AUTO) -------------------------------------------
    def start_test(self, start, to, back, record, hold, band):
        if self.busy():
            return 'a test is already running'
        r = self.send('PRES SP %.1f;MODE AUTO' % start)
        if r:
            return r
        self.test = {'start': start, 'to': to, 'back': back, 'record': record, 'hold': hold, 'band': band,
                     'phase': 'settle', 'since': None, 't_phase': None, 'rows': [], 'steps': []}
        self.note('setpoint step test: settling at %.1f mbar' % start)
        return ''

    def stop_test(self, why='stopped'):
        if self.test:
            self.note('setpoint step test %s; setpoint back to %.1f mbar' % (why, self.test['start']))
            self.cmds.put(('set', 'PRES SP %.1f' % self.test['start'], queue.Queue()))
            self.test = None

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
                if t - T['since'] >= T['hold']:
                    T['steps'] = [(T['start'], T['to'])] + ([(T['to'], T['start'])] if T['back'] else [])
                    self.next_step(t)
            else:
                T['since'] = None
                if t - T['t_phase'] > 1200:
                    self.stop_test('ended: %.1f mbar not held within 20 min' % T['start'])
            return
        if p is not None:
            T['rows'].append((t - T['t_phase'], p))
        if t - T['t_phase'] >= T['record']:
            p0, p1 = T['cur']
            m = metrics(T['rows'], p0, p1, T['band'])
            if m:
                with self.lock:
                    self.results.append(m)
                self.note('%.1f -> %.1f mbar: 90 %% in %s s, overshoot %s %%, settled in %s s' %
                          (p0, p1, m['t90_s'], m['overshoot_pct'], m['settle_s']))
            if T['steps']:
                self.next_step(t)
            else:
                self.note('setpoint step test done')
                self.test = None

    def next_step(self, t):
        T = self.test
        T['cur'] = T['steps'].pop(0)
        T['phase'] = '%.1f->%.1f mbar' % T['cur']
        T['t_phase'], T['rows'] = t, []
        r = self.ask('PRES SP %.1f;PRES SP?' % T['cur'][1])   # the query: a set alone gives no reply
        if r.startswith('ERR'):
            self.note('setpoint refused: ' + r)
        self.note('step %.1f -> %.1f mbar, recording %d s' % (T['cur'][0], T['cur'][1], T['record']))

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
        self.vtest = {'at': at, 'step': step, 'hold': hold, 'record': record, 'phase': 'valve settle',
                      't_phase': None, 'rows': [], 'moves': [(at, at + step), (at + step, at)], 'base': []}
        self.note('valve step test: valve to %.1f %%, waiting %d s for the pressure to settle' % (at, hold))
        return ''

    def stop_vtest(self, why='stopped'):
        if self.vtest:
            self.note('valve step test %s; valve left where it is, in MANUAL' % why)
            self.vtest = None

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
            else:
                m = {'from': x0, 'to': x1, 'p0': round(base, 3), 'dp': round(dp[-1], 3) if dp else None,
                     'K': None, 'delay_s': None, 'tau_s': None, 'kp': None, 'ti': None}
                self.note('valve %.1f -> %.1f %%: no usable response (pressure moved too little, '
                          'or the wrong way); try a larger step' % (x0, x1))
            with self.lock:
                self.vresults.append(m)
        if not V['moves']:
            self.note('valve step test done; valve back at %.1f %%, MANUAL' % V['at'])
            self.vtest = None
            return
        V['cur'] = V['moves'].pop(0)
        V['p0'] = statistics.mean(V['base']) if V['base'] else p
        V['phase'] = 'valve %.1f->%.1f %%' % V['cur']
        V['t_phase'], V['rows'] = t, []
        r = self.ask('VALV SET %.1f;VALV?' % V['cur'][1])
        if r.startswith('ERR'):
            self.stop_vtest('ended: ' + r)
            return
        self.note('valve %.1f -> %.1f %%, recording %d s' % (V['cur'][0], V['cur'][1], V['record']))

    # Gain table (pmc_gains.py tune) ----------------------------------------
    def start_tune(self, sps, step, tc):
        if self.busy():
            return 'a test is already running'
        sps = sorted(s for s in sps if s > 0)
        if not sps:
            return 'give at least one setpoint'
        a = SimpleNamespace(hold=120, settle_max=1200, step_max=900, step_limit=5.0, min_dp=0.05,
                            max_dp=0.3, tc=tc, sp=sps, step=step)
        self.job, self.stop_job = 'gain table', False
        threading.Thread(target=self.tune_job, args=(a,), daemon=True).start()
        return ''

    def tune_job(self, a):
        link = ViaBoard(self)
        t = GuiTuner(link, pmc_gains.Clock(self.speed), a, None)
        t.board, t.x0 = self, None
        sp0 = mode0 = None
        rows, step = [], a.step
        try:
            sp0 = pmc_gains.value(link.ask('PRES SP?')[0], 'PRES SP')
            mode0 = t.sample()[2]
            t.say('found setpoint %s mbar, %s; testing %s mbar' % (sp0, mode0, ', '.join('%g' % s for s in a.sp)))
            for sp in a.sp:
                rows.append(t.tune_point(sp, step))
                self.set_table([(r['sp_mbar'], r['kp'], r['ti_min']) for r in rows], rows, 'measured')
                K = rows[-1]['K_mbar_per_pct']
                nxt = a.sp[min(len(rows), len(a.sp) - 1)]
                step = min(max(round(0.10 * nxt / K, 1), 0.1), a.step_limit)
            t.say('done')
        except Stopped:
            t.say('stopped')
        except (Refused, RuntimeError, ConnectionError, ValueError) as e:
            t.say('stopped: %s' % e)
        finally:
            self.stop_job = False
            back = []
            if t.x0 is not None:
                back.append('VALV SET %.1f' % t.x0)
            if sp0 is not None:
                back.append('PRES SP %.1f' % sp0)
            if mode0 == 'AUTO':
                back.append('MODE AUTO')
            for c in back:
                r = self.send(c)
                t.say('put back: %s%s' % (c, ' (' + r + ')' if r else ''))
            self.job = None

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
        if not self.schedule or not self.table or sp is None or self.job or sp == self.sched_sp:
            return
        kp, ti = gains_for(self.table, sp)
        now = self.state['pid']
        if not now or (round(now[0], 2), round(now[1], 2)) != (kp, ti):
            r = self.ask('PID SET %.2f,%.2f;PID?' % (kp, ti))
            if r.startswith('ERR'):
                self.note('gain schedule off: %s' % r)
                self.schedule = False
                return
            self.note('gain schedule: setpoint %g mbar, KP %.2f TI %.2f' % (sp, kp, ti))
        self.sched_sp = sp

    # For the page --------------------------------------------------------
    def snapshot(self, since):
        with self.lock:
            rows = [r for r in self.hist if r[0] > since]
            blended = [[sp] + list(gains_for(self.table, sp)) for sp in SHOW_SP] if self.table else []
            return {'link': self.state['link'], 'idn': self.state['idn'], 'pid': self.state['pid'],
                    'log': self.state['log'], 'rows': rows, 'results': self.results, 'vresults': self.vresults,
                    'test': (self.test['phase'] if self.test else self.vtest['phase'] if self.vtest
                             else self.job),
                    'table': self.table_rows, 'table_name': self.table_name, 'blended': blended,
                    'schedule': self.schedule}

    def csv_text(self):
        f = io.StringIO()
        w = csv.writer(f)
        w.writerow(['t_s', 'sp_mbar', 'p_mbar', 'valve_pct', 'mode', 'alarm', 'phase'])
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
            stamp = time.strftime('%Y%m%d_%H%M%S')
            if self.path == '/':
                self.out(page, 'text/html; charset=utf-8')
            elif self.path.startswith('/api/state'):
                since = float(self.path.partition('since=')[2] or -1)
                self.out(json.dumps(board.snapshot(since)))
            elif self.path == '/api/csv':
                self.out(board.csv_text(), 'text/csv',
                         {'Content-Disposition': 'attachment; filename="pmc_%s.csv"' % stamp})
            elif self.path == '/api/table.csv':
                self.out(board.table_csv(), 'text/csv',
                         {'Content-Disposition': 'attachment; filename="gains_%s.csv"' % stamp})
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
                    r = board.start_test(float(q['start']), float(q['to']), bool(q.get('back')),
                                         float(q.get('record', 600)), float(q.get('hold', 120)),
                                         float(q.get('band', 0.1)))
                elif p == '/api/vtest':
                    r = board.start_vtest(float(q['at']), float(q['step']), float(q.get('hold', 180)),
                                          float(q.get('record', 600)))
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
                elif p == '/api/test/stop':
                    board.stop_test()
                    board.stop_vtest()
                    if board.job:
                        board.stop_job = True
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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('host', help='board IP, or IP:port (port 8888 by default)')
    ap.add_argument('--port', type=int, default=8080, help='web page port on this PC (default 8080)')
    ap.add_argument('--bind', default='127.0.0.1',
                    help='address the page listens on (default this PC only; 0.0.0.0 lets other PCs open it)')
    ap.add_argument('--speed', type=float, default=1.0, help=argparse.SUPPRESS)   # simulator only
    a = ap.parse_args()
    board = Board(a.host, a.speed)
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
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    sys.exit(main())
