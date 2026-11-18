# Frappy configuration for one iPC pressure controller.
#
# Set the board's address in IPC_URI (or edit the default below): the
# address shown on the board's screen, port 8888. The board takes one
# connection at a time, so close any terminal or logger on port 8888 first.
#
# Uses the firmware's whole-word text commands (SETPOINT, MODE AUTO, STOP,
# ...); checked against firmware 0.18.0.

import os

Node('ipc.cryostat',
     'iPC pot pressure controller (needle valve)',
     'tcp://10767',
)

Mod('ipc_io',
    'frappy_ipc.ipc.IpcIO',
    'text link to the iPC',
    uri=os.environ.get('IPC_URI', 'tcp://192.168.1.50:8888'),
    export=False,   # no raw text commands over SECoP
)

Mod('pressure',
    'frappy_ipc.ipc.Pressure',
    'pot pressure; writing the setpoint starts AUTO',
    io='ipc_io',
    output_module='valve',
    pollinterval=1,
    tolerance=0.5,
)

Mod('valve',
    'frappy_ipc.ipc.Valve',
    'needle valve position; writing it ends AUTO',
    io='ipc_io',
    pollinterval=2,
)

Mod('room_pressure',
    'frappy_ipc.ipc.RoomPressure',
    'room pressure (Pressure 21 Click); leave out if not fitted',
    io='ipc_io',
    pollinterval=10,
)
