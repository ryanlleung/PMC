#ifndef _PMC_CONFIG_H_
#define _PMC_CONFIG_H_

/**
 * @brief Build options.
 *
 * PMC_LINK_ETHERNET
 *   1: the board. The text commands go over Ethernet (TCP port
 *      PMC_TCP_PORT) and SYSCLK runs at 150 MHz for the 50 MHz RMII clock.
 *   0: a host build (the simulator, the screen previews, the host tests):
 *      no Ethernet driver; the host supplies the link itself (link.h).
 * Flashing is over USB-C with mikroBootloader. There is no USB COM port
 * build from 0.16.28.
 */
#ifndef PMC_LINK_ETHERNET
#define PMC_LINK_ETHERNET 1
#endif

// Set commands on the text link and Modbus writes need Menu > System >
// Remote control On (0.13.4; On by default from 0.13.7). That switch is the
// only gate from 0.16.28: the build flags PMC_LINK_WRITE, PMC_MODBUS_WRITE
// and PMC_REMOTE_SWITCH are gone.

// The text link takes the remote command set only: what an operator can
// use and judge away from the system (remote.h). Homing, calibration,
// settings, tuning, saving and the motor are on the touch screen.

// Shown on screen and returned by VER?, with the build date and time.
#define PMC_FW_VERSION    "0.16.31"

// Valve limit switch: one NC dry contact to GND on S1 TX, PD5 (Adapter Click
// IDC10 pad 8, printed RX: the Adapter labels from the Click's side; 0.14.6).
// S1 RX PA3 in 0.14.3-0.14.5, S1 PWM PD12 in 0.14.2, S2 PWM PD13 before.
// Pin level that means the contact is OPEN (at the limit or a broken wire).
// 1 for the bare contact with the pull-up; 0 for an inverting interface.
#define PMC_LIMIT_OPEN_LEVEL 1
// 1: MCU internal pull-up on the input (needed for the bare contact).
#define PMC_LIMIT_PULLUP  1
// 1: the limit switch is the closing stop: closing moves are refused, and
// stopped mid-move, while it reads open. 0: its state is only displayed.
#define PMC_LIMIT_STOP    1


// Room pressure (0.11.47): a Pressure 21 Click (BMP581, I2C2 0x46) on the
// Adapter Click in S1. 1: probed at start-up and read once a second
// (BAROMETER?, baro.c); nothing changes if it is not fitted.
// 0: not built in.
#define PMC_BARO          1

// The text command port (8888 from 0.13.0; 5000 before).
#define PMC_TCP_PORT      8888
#define PMC_HOSTNAME      "pmc"
// Modbus TCP server (0.12.0, Ethernet build only): FC1, FC3, FC5 and FC16,
// register map in docs/modbus-register-map.md. A new client past
// PMC_MODBUS_CLIENTS replaces the one idle longest; one that stops in the
// middle of a request is dropped after PMC_MODBUS_IDLE_MS.
#define PMC_MODBUS_PORT    502
#define PMC_MODBUS_CLIENTS 2
#define PMC_MODBUS_IDLE_MS 5000
// Target, valve % and the command coils can be written (Ryan, 26 Oct), with
// the same checks as the screen, while Remote control is On. Anyone who can
// reach port 502 can then run the valve. Off: every write answers
// exception 1.

// The controller's address when none is saved under Menu > System
// (static only, no DHCP from 0.14.9). A conventional private address
// (0.14.11; link-local 169.254.10.50/16 to 0.14.9, 169.254.1.1/16 in
// 0.14.10): a PC talks to it from 192.168.1.x/24 with a fixed address.
#define PMC_IP            "192.168.1.100"
#define PMC_MASK          "255.255.255.0"


#endif // _PMC_CONFIG_H_
