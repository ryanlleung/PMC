#ifndef _IPC_CONFIG_H_
#define _IPC_CONFIG_H_

/**
 * @brief Build options.
 *
 * IPC_LINK_ETHERNET
 *   1: the board. The text commands go over Ethernet (TCP port
 *      IPC_TCP_PORT) and SYSCLK runs at 150 MHz for the 50 MHz RMII clock.
 *   0: a host build (the simulator, the screen previews, the host tests):
 *      no Ethernet driver; the host supplies the link itself (link.h).
 * Flashing is over USB-C with mikroBootloader.
 */
#ifndef IPC_LINK_ETHERNET
#define IPC_LINK_ETHERNET 1
#endif

// Set commands on the text link and Modbus writes need Menu > System >
// Network control On (On by default). That switch is the only gate.

// The text link takes the remote command set only: what an operator can
// use and judge away from the system (remote.h). Homing, calibration,
// settings, tuning, saving and the motor are on the touch screen.

// Shown on screen with the build date and time, and returned by *IDN?.
#define IPC_FW_VERSION    "0.18.0"

// Valve limit switch: one NC dry contact to GND on S1 TX, PD5 (Adapter Click
// IDC10 pad 8, printed RX: the Adapter labels from the Click's side).
// Pin level that means the contact is OPEN (at the limit or a broken wire).
// 1 for the bare contact with the pull-up; 0 for an inverting interface.
#define IPC_LIMIT_OPEN_LEVEL 1
// 1: MCU internal pull-up on the input (needed for the bare contact).
#define IPC_LIMIT_PULLUP  1
// 1: the limit switch is the closing stop: closing moves are refused, and
// stopped mid-move, while it reads open. 0: its state is only displayed.
#define IPC_LIMIT_STOP    1


// Room pressure: a Pressure 21 Click (BMP581, I2C2 0x46) on the
// Adapter Click in S1. 1: probed at start-up and read once a second
// (BAROMETER?, baro.c); nothing changes if it is not fitted.
// 0: not built in.
#define IPC_BARO          1

// The text command port.
#define IPC_TCP_PORT      8888
#define IPC_HOSTNAME      "ipc"
// Modbus TCP server (Ethernet build only): FC1, FC3, FC5 and FC16,
// register map in docs/modbus-register-map.md. A new client past
// IPC_MODBUS_CLIENTS replaces the one idle longest; one that stops in the
// middle of a request is dropped after IPC_MODBUS_IDLE_MS.
#define IPC_MODBUS_PORT    502
#define IPC_MODBUS_CLIENTS 2
#define IPC_MODBUS_IDLE_MS 5000
// Target, valve % and the command coils can be written, with the same
// checks as the screen, while Network control is On. Anyone who can
// reach port 502 can then run the valve. Off: every write answers
// exception 1.

// The controller's address when none is saved under Menu > System
// (static only, no DHCP). A conventional private address: a PC talks to it
// from 192.168.1.x/24 with a fixed address.
#define IPC_IP            "192.168.1.100"
#define IPC_MASK          "255.255.255.0"


#endif // _IPC_CONFIG_H_
