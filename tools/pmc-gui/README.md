# Control test GUI

`pmc_gui.py` is a small web server on a PC that talks to the board's text port (TCP 8888) and serves one page to a browser:

- the pressure, setpoint and valve, with a live chart (5 min to 4 hours) and the mode and alarms;
- Set setpoint, Start AUTO, MANUAL, Go to % and STOP;
- the PID gains in use (`PID?`), and Set and save (`PID SET`, firmware 0.15.8 and later);
- setpoint step tests: AUTO at From, held within the band for 2 minutes, then a step to To (and back), with the time to 90 %, the overshoot, the settling time and the last minute's mean and spread;
- Download CSV: every sample, one a second.

## Run it with Python

Python 3.8 or later, standard library only, so nothing needs pip or admin rights. On Windows without admin, the "Windows embeddable package" zip from python.org runs after unzipping.

```
python pmc_gui.py 192.168.1.100
```

Then open http://localhost:8080. `--port 8090` picks another page port.

## Run it with Docker

```
docker build -t pmc-gui tools/pmc-gui
docker run --rm -p 8080:8080 pmc-gui 192.168.1.100
```

Then open http://localhost:8080. The container reaches the board through the PC's network.

## Before you start

- Settings > System setup > Remote control On, or every button answers ERR 211.
- The board takes one text-port connection at a time, and a new one replaces the old. Close PuTTY, the logger and `pmc_gains.py run` first.
- STOP on the page and on the screen both end AUTO. If the board leaves AUTO during a step test (a fault, W-2, W-4, W-5, W-6 or STOP), the test ends and says why.
- Every Set and save that changes a gain is written to flash. Set gains when they change, not in a loop.

To try it without the board, run `node simulator/tests/tcp-board.js` and use `127.0.0.1` as the address.
