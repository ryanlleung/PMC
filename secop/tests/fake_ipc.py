"""A stand-in iPC text port for testing the Frappy driver without a board.

    python fake_ipc.py [port]

It answers as firmware 0.18.0 does on the text port: no echo, nothing sent
unasked, ';' chaining, and one reply line per command: a query gives its
words and value (PRESSURE 4.625 mbar), a set command nothing unless refused,
and a refusal ERR <code> <text>.

Only the commands the driver uses are modelled. The controller is on and
the valve homed at start, and the pressure follows the valve with a lag
(0.2 mbar per % open). For the tests, TEST lines (never on a board) put it
in the states that only the screen reaches:

    TEST OFF            off, as at power-up (VALVE NONE: not homed)
    TEST ON             turned on at the screen (MANUAL, homed)
    TEST SHUTDOWN       Shut down: the valve closes, then OFF
    TEST REMOTE 0|1     Menu > System > Network control Off or On
    TEST FAULT <tag>    a latched fault (F-1 ...); ALARM RESET clears it
"""

import socket
import sys
import threading
import time

VERSION = '0.18.0'


class Board:
    def __init__(self):
        self.t0 = time.time()
        self.p = 1.0
        self.valve = 5.0
        self.goal = 5.0
        self.homed = True
        self.sp = 5.0
        self.mode = 'MANUAL'
        self.alarm = ''
        self.remote = True
        self.lock = threading.Lock()

    def step(self, dt):
        with self.lock:
            if self.mode == 'AUTO':
                self.goal = min(100.0, max(0.0, self.goal + 2.0 * (self.sp - self.p) * dt))
            d = self.goal - self.valve
            self.valve += max(-5 * dt, min(5 * dt, d))
            self.p += (0.2 * self.valve - self.p) * min(1.0, dt / 3.0)
            if self.mode == 'SHUTDOWN' and self.valve <= 0.0:
                self.mode = 'OFF'

    def chain(self, line):
        """The reply to a line of commands separated by ';': the replies of
        its queries, joined by ';'. Set commands answer only when refused, and
        at the first ERR the rest is skipped. None if there is nothing to say."""
        replies = []
        for part in line.split(';'):
            part = part.strip()
            reply = self.command(part)
            if reply is None:
                continue
            replies.append(reply)
            if reply.startswith('ERR'):
                break
        return ';'.join(replies) if replies else None

    def off_why(self):
        if self.mode == 'OFF':
            return 'ERR 213 Controller is off'
        if self.mode == 'SHUTDOWN':
            return 'ERR 213 Shutting down, STOP to cancel'
        return None

    def command(self, line):
        """The reply to one command: a query's line, None for a set command
        that was taken, or ERR <code> <text>."""
        with self.lock:
            if line == '*IDN?':
                return f'MercShore,iPC,3600273C3438510B00410040,{VERSION}'
            if line == 'PRESSURE?':
                return f'PRESSURE {self.p:.3f} mbar'
            if line == 'SETPOINT?':
                return f'SETPOINT {self.sp:.1f} mbar'
            if line == 'MODE?':
                return f'MODE {self.mode}'
            if line == 'VALVE?':
                return f'VALVE {self.valve:.2f} %' if self.homed else 'VALVE NONE'
            if line == 'VALVE MOVING?':
                return f'VALVE MOVING {int(abs(self.goal - self.valve) > 0.005)}'
            if line == 'BAROMETER?':
                return 'BAROMETER 1013.25 mbar'
            if line == 'ALARM?':
                return f'ALARM {self.alarm},1,Test fault' if self.alarm else 'ALARM NONE'
            if line.startswith('TEST '):
                return self.test(line[5:])
            if not self.remote:
                return 'ERR 211 Read only over the network'
            if line.startswith('SETPOINT '):
                try:
                    v = float(line[9:])
                except ValueError:
                    return 'ERR 101 Expected: SETPOINT <mbar>'
                if not 0 <= v <= 1034.2:
                    return 'ERR 102 Outside 0 to 1034.2 mbar'
                self.sp = round(v, 1)
                return None
            if line == 'MODE AUTO':
                if self.off_why():
                    return self.off_why()
                if self.alarm.startswith('F'):
                    return 'ERR 203 Reset the fault first'
                self.mode = 'AUTO'
                return None
            if line == 'MODE MANUAL':
                if self.off_why():
                    return self.off_why()
                if self.mode == 'AUTO':
                    self.goal = self.valve
                self.mode = 'MANUAL'
                return None
            if line.startswith('VALVE SET '):
                try:
                    v = float(line[10:])
                except ValueError:
                    return 'ERR 101 Expected: VALVE SET <%>'
                if not 0 <= v <= 100:
                    return 'ERR 102 Outside 0 to 100 %'
                if self.off_why():
                    return self.off_why()
                if self.mode == 'AUTO':
                    return 'ERR 202 Switch to MANUAL first'
                self.goal = v
                return None
            if line == 'STOP':
                self.goal = self.valve
                if self.mode in ('AUTO', 'SHUTDOWN'):
                    self.mode = 'MANUAL'
                return None
            if line == 'ALARM RESET':
                self.alarm = ''
                return None
            if line == 'ALARM OK':
                return None
        return 'ERR 100 Unknown command'

    def test(self, what):
        """A state only the screen reaches (stand-in only)."""
        if what == 'OFF':
            self.mode, self.homed = 'OFF', False
        elif what == 'ON':
            self.mode, self.homed = 'MANUAL', True
        elif what == 'SHUTDOWN':
            self.mode, self.goal = 'SHUTDOWN', 0.0
        elif what in ('REMOTE 0', 'REMOTE 1'):
            self.remote = what.endswith('1')
        elif what.startswith('FAULT '):
            self.alarm = what[6:]
            if self.mode == 'AUTO':
                self.mode, self.goal = 'MANUAL', self.valve
        else:
            return 'ERR 100 Unknown command'
        return None


def serve(port):
    board = Board()

    def physics():
        last = time.time()
        while True:
            time.sleep(0.1)
            now = time.time()
            board.step(now - last)
            last = now

    threading.Thread(target=physics, daemon=True).start()
    srv = socket.create_server(('127.0.0.1', port))
    print(f'fake iPC on port {port}', flush=True)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=client, args=(board, conn), daemon=True).start()


def client(board, conn):
    try:
        buf = b''
        while True:
            data = conn.recv(1024)
            if not data:
                break
            buf += data
            while b'\n' in buf or b'\r' in buf:
                i = min(x for x in (buf.find(b'\n'), buf.find(b'\r')) if x >= 0)
                line, buf = buf[:i].decode().strip(), buf[i + 1:]
                if line:
                    reply = board.chain(line)
                    if reply is not None:
                        conn.sendall((reply + '\r\n').encode())
    except OSError:
        pass
    finally:
        conn.close()


if __name__ == '__main__':
    serve(int(sys.argv[1]) if len(sys.argv) > 1 else 8888)
