#ifndef _PMC_CONFIG_H_
#define _PMC_CONFIG_H_

/**
 * @brief Build options.
 *
 * PMC_LINK_ETHERNET
 *   1: the text commands go over Ethernet (TCP port PMC_TCP_PORT).
 *      SYSCLK runs at 150 MHz for the 50 MHz RMII clock, and the USB COM
 *      port is off (the two clocks cannot share the PLL).
 *   0: USB COM port (COM3) as before, Ethernet off.
 * Flashing over USB-C with mikroBootloader works the same either way.
 * 1 by default from 0.12.18 (Ryan, 27 Oct); 0 for a bench build on the
 * USB COM port.
 */
#ifndef PMC_LINK_ETHERNET
#define PMC_LINK_ETHERNET 1
#endif

// Ethernet build only (0.11.46): 0 = read only, the network link answers
// queries (ending in ?) and refuses every set command with ERR 211. 1 = the
// whole remote command set, as on the USB COM port (Ryan, 26 Oct):
// target, MANUAL/AUTO, valve %, STOP, the alarm reset and OK, the clock.
#define PMC_LINK_WRITE    1

// Remote control switch (0.13.4, Ryan 28 Oct, network plan A): set commands
// on the text link need Settings > System > Remote control On, as
// do Modbus writes (PMC_MODBUS_WRITE). On by default from 0.13.7. PMC_LINK_WRITE and
// PMC_MODBUS_WRITE 0 still mean never. 1 for the Ethernet build; the USB
// link is local and not switched. The simulator builds with 1, as the board.
#ifndef PMC_REMOTE_SWITCH
#define PMC_REMOTE_SWITCH PMC_LINK_ETHERNET
#endif

// The text link takes the remote command set only (0.13.0, Ryan 27 Oct):
// what an operator can use and judge away from the system (remote.h).
// Homing, calibration, settings, tuning, saving and the motor are on the
// touch screen. 1: also the engineering commands under their old names
// (VALVE CAL, CAL VAC, PID KP, CFG SAVE, MOT ..., TUNE ...), for a bench
// build and the simulator's tests; never for the cryostat.
#ifndef PMC_ENG_COMMANDS
#define PMC_ENG_COMMANDS  0
#endif

// Shown on screen and returned by VER?, with the build date and time.
#define PMC_FW_VERSION    "0.14.6"

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
// (BARO?, baro.c); nothing changes if it is not fitted.
// 0: not built in.
#define PMC_BARO          1

// Serial number of the Druck in use, shown on the Calibration tab.
#define PMC_DRUCK_SERIAL  "5880156"

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
// 1 (Ryan, 26 Oct): target, valve % and the command coils can be written,
// with the same checks as the screen. Anyone who can reach port 502 can
// then run the valve. 0: read only, every write answers exception 1.
#ifndef PMC_MODBUS_WRITE
#define PMC_MODBUS_WRITE   1
#endif

// Address used if no DHCP server answers within PMC_DHCP_TIMEOUT_MS.
// Link-local (169.254.0.0/16), the range Windows gives itself on a direct
// cable with no DHCP, so a laptop plugged straight in can reach it.
#define PMC_FALLBACK_IP   "169.254.10.50"
#define PMC_FALLBACK_MASK "255.255.0.0"
#define PMC_DHCP_TIMEOUT_MS 10000


#endif // _PMC_CONFIG_H_
