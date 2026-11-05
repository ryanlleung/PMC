#!/usr/bin/env python3
"""Control test GUI for the pressure controller, in a web browser.

    python pmc_gui.py 192.168.1.100          # then open http://localhost:8080
    python pmc_gui.py 192.168.1.100 --port 8090

A small web server on this PC talks to the board's text port (TCP 8888) and
serves one page: live pressure, setpoint and valve with a chart, the mode
and alarms, setpoint / AUTO / MANUAL / STOP, the PID gains (PID? and
PID SET), valve step tests in MANUAL (the plant's gain, delay and time
constant, and the KP and TI they suggest), and setpoint step tests in AUTO
(overshoot and settling time).
The chart's samples download as CSV.

Python 3.8 or later, standard library only: nothing to install, and no
admin rights needed. The board takes one text-port connection at a time,
so close PuTTY, the logger and pmc_gains.py run first. Set commands need
Settings > System setup > Remote control On.
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
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BOARD_PORT = 8888
HISTORY = 4 * 3600          # samples kept (1 a second): 4 hours
HERE = os.path.dirname(os.path.abspath(__file__))
# The step fit and SIMC rules of the gain schedule program (tools/gain-schedule).
for d in (os.path.join(HERE, '..', 'gain-schedule'), HERE):
    if os.path.exists(os.path.join(d, 'pmc_gains.py')):
        sys.path.insert(0, d)
        break
from pmc_gains import fit_step, simc   # noqa: E402


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


class Board:
    """One connection, polled once a second from its own thread. Everything
    else hands it lines through a queue, so the board sees one client."""

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
        self.test = None          # the setpoint step test in progress
        self.results = []
        self.vtest = None         # the valve step test in progress
        self.vresults = []

    # Link ----------------------------------------------------------------
    def connect(self):
        self.sock = socket.create_connection(self.addr, timeout=5)
        self.buf = b''
        self.state['idn'] = self.ask('*IDN?')
        self.state['link'] = 'connected'

    def ask(self, line):
        self.sock.sendall(line.encode() + b'\n')
        while b'\r\n' not in self.buf:
            d = self.sock.recv(1024)
            if not d:
                raise ConnectionError('the board closed the connection')
            self.buf += d
        r, self.buf = self.buf.split(b'\r\n', 1)
        return r.decode(errors='replace')

    def send(self, line):
        """A set command from the page; the reply (or '' when accepted) comes back."""
        done = queue.Queue()
        self.cmds.put((line, done))
        try:
            return done.get(timeout=10)
        except queue.Empty:
            return 'no reply from the board'

    def note(self, text):
        with self.lock:
            self.state['log'] = (self.state['log'] + [time.strftime('%H:%M:%S ') + text])[-30:]

    # Loop ----------------------------------------------------------------
    def run(self):
        while True:
            try:
                if self.sock is None:
                    self.connect()
                    self.note('connected: ' + self.state['idn'])
                self.tick()
            except (OSError, ConnectionError) as e:
                self.state['link'] = 'no link: %s' % e
                self.note('link lost: %s' % e)
                if self.sock:
                    self.sock.close()
                self.sock = None
                time.sleep(2 / self.speed)
                continue
            time.sleep(1.0 / self.speed)

    def tick(self):
        while not self.cmds.empty():
            line, done = self.cmds.get()
            r = self.ask(line + ';MODE?')     # a query so every line gets one reply
            parts = r.split(';')
            err = [x for x in parts if x.startswith('ERR')]
            reply = err[0] if err else ''
            self.note('%s  %s' % (line, reply or 'ok'))
            done.put(reply)
        r = self.ask('PRES SP?;PRES?;VALV?;MODE?;ALRM?;PID?')
        sp, p, v, m, a, pid = (r.split(';') + [''] * 6)[:6]
        t = round((time.time() - self.t0) * self.speed, 1)
        phase = self.test['phase'] if self.test else self.vtest['phase'] if self.vtest else ''
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

    # Step test -----------------------------------------------------------
    def start_test(self, start, to, back, record, hold, band):
        if self.test or self.vtest:
            return 'a test is already running'
        r = self.send('PRES SP %.1f;MODE AUTO' % start)
        if r:
            return r
        self.test = {'start': start, 'to': to, 'back': back, 'record': record, 'hold': hold, 'band': band,
                     'phase': 'settle', 'since': None, 't_phase': None, 'rows': [], 'steps': []}
        self.note('step test: settling at %.1f mbar' % start)
        return ''

    def stop_test(self, why='stopped'):
        if self.test:
            self.note('step test %s; setpoint back to %.1f mbar' % (why, self.test['start']))
            self.cmds.put(('PRES SP %.1f' % self.test['start'], queue.Queue()))
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
                self.note('step test done')
                self.test = None

    def next_step(self, t):
        T = self.test
        T['cur'] = T['steps'].pop(0)
        T['phase'] = '%.1f->%.1f' % T['cur']
        T['t_phase'], T['rows'] = t, []
        r = self.ask('PRES SP %.1f;PRES SP?' % T['cur'][1])   # the query: a set alone gives no reply
        if r.startswith('ERR'):
            self.note('setpoint refused: ' + r)
        self.note('step %.1f -> %.1f mbar, recording %d s' % (T['cur'][0], T['cur'][1], T['record']))

    # Valve step test (MANUAL): the plant alone, before the loop -----------
    # The valve to At %, a wait for the pressure to settle, then At + Step %
    # and back, each recorded and fitted to a gain, a delay and a time
    # constant (pmc_gains.fit_step), with the KP and TI the SIMC rules give.
    def start_vtest(self, at, step, hold, record):
        if self.test or self.vtest:
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
                kp, ti = simc(f['K'], f['delay'], f['tau'])
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
        V['phase'] = 'valve %.1f->%.1f' % V['cur']
        V['t_phase'], V['rows'] = t, []
        r = self.ask('VALV SET %.1f;VALV?' % V['cur'][1])
        if r.startswith('ERR'):
            self.stop_vtest('ended: ' + r)
            return
        self.note('valve %.1f -> %.1f %%, recording %d s' % (V['cur'][0], V['cur'][1], V['record']))

    # For the page --------------------------------------------------------
    def snapshot(self, since):
        with self.lock:
            rows = [r for r in self.hist[-HISTORY:] if r[0] > since]
            return {'link': self.state['link'], 'idn': self.state['idn'], 'pid': self.state['pid'],
                    'log': self.state['log'], 'rows': rows, 'results': self.results,
                    'vresults': self.vresults,
                    'test': self.test['phase'] if self.test else self.vtest['phase'] if self.vtest else None}

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
            if self.path == '/':
                self.out(page, 'text/html; charset=utf-8')
            elif self.path.startswith('/api/state'):
                since = float(self.path.partition('since=')[2] or -1)
                self.out(json.dumps(board.snapshot(since)))
            elif self.path == '/api/csv':
                self.out(board.csv_text(), 'text/csv',
                         {'Content-Disposition': 'attachment; filename="pmc_%s.csv"' % time.strftime('%Y%m%d_%H%M%S')})
            else:
                self.send_error(404)

        def do_POST(self):
            n = int(self.headers.get('Content-Length') or 0)
            try:
                q = json.loads(self.rfile.read(n) or b'{}')
                if self.path == '/api/cmd':
                    r = board.send(str(q['line']).upper()[:90])
                elif self.path == '/api/test':
                    r = board.start_test(float(q['start']), float(q['to']), bool(q.get('back')),
                                         float(q.get('record', 600)), float(q.get('hold', 120)),
                                         float(q.get('band', 0.1)))
                elif self.path == '/api/vtest':
                    r = board.start_vtest(float(q['at']), float(q['step']), float(q.get('hold', 180)),
                                          float(q.get('record', 600)))
                elif self.path == '/api/test/stop':
                    board.stop_test()
                    board.stop_vtest()
                    r = ''
                else:
                    self.send_error(404)
                    return
            except (ValueError, KeyError, TypeError) as e:
                r = 'bad request: %s' % e
            self.out(json.dumps({'reply': r}))

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('host', help='board IP, or IP:port (port 8888 by default)')
    ap.add_argument('--port', type=int, default=8080, help='web page port on this PC (default 8080)')
    ap.add_argument('--bind', default='127.0.0.1',
                    help='address the page listens on (default this PC only; 0.0.0.0 in Docker)')
    ap.add_argument('--speed', type=float, default=1.0, help=argparse.SUPPRESS)   # simulator only
    a = ap.parse_args()
    board = Board(a.host, a.speed)
    threading.Thread(target=board.run, daemon=True).start()
    srv = ThreadingHTTPServer((a.bind, a.port), make_handler(board))
    print('board %s:%d, open http://localhost:%d  (Ctrl+C to stop)' % (board.addr + (a.port,)))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    sys.exit(main())
