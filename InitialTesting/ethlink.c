#include "pmc_config.h"

#if PMC_LINK_ETHERNET

#include <stdarg.h>
#include <string.h>

#include "core/net.h"
#include "core/tcp.h"
#include "drivers/mac/stm32f4xx_eth_driver.h"
#include "drivers/phy/lan8720_driver.h"
#include "dhcp/dhcp_client.h"

#include "lvgl.h"   // lv_vsnprintf, lv_strlcpy
#include "eth_hw.h"
#include "ethlink.h"

/* --------------------------------------------------------------------------
 * One TCP server on PMC_TCP_PORT, one client at a time (a new connection
 * replaces the old one). Text lines both ways, as on the USB COM port.
 * CycloneTCP runs without an RTOS: ethlink_task() calls netTask() from the
 * main loop and every socket call uses a zero timeout.
 * ------------------------------------------------------------------------ */

static NetInterface *ifc;
static DhcpClientContext dhcp;
static Socket *listener;
static Socket *client;
static bool up;
static bool fallback;
static int phy_addr = ETH_HW_NO_PHY;
static uint32_t phy_id;
static char ip_text[16] = "";
static char status_text[80] = "Ethernet: not started";
static const char *chip_err = "NET";   // short fault for the header

// Called by the MAC driver (overrides the library's weak default).
void stm32f4xxEthInitGpio(NetInterface *interface)
{
    (void)interface;
    eth_hw_init_pins();
}

static void dhcp_timeout(DhcpClientContext *context, NetInterface *interface)
{
    Ipv4Addr a, m;
    dhcpClientStop(context);
    ipv4StringToAddr(PMC_FALLBACK_IP, &a);
    ipv4StringToAddr(PMC_FALLBACK_MASK, &m);
    ipv4SetHostAddr(interface, a);
    ipv4SetSubnetMask(interface, m);
    fallback = true;
}

void ethlink_init(void)
{
    MacAddr mac;
    uint8_t uid[12];

    if (!eth_hw_clock_50mhz()) {
        lv_strlcpy(status_text, "Ethernet: HSE crystal did not start", sizeof status_text);
        chip_err = "NET HSE";
        return;
    }
    eth_hw_init_pins();
    phy_addr = eth_hw_probe(&phy_id);
    if (phy_addr == ETH_HW_NO_REFCLK) {
        lv_strlcpy(status_text, "Ethernet: no 50 MHz RMII clock (MAC reset stuck)", sizeof status_text);
        chip_err = "NET NO CLK";
        return;
    }
    if (phy_addr == ETH_HW_NO_PHY) {
        lv_strlcpy(status_text, "Ethernet: PHY not answering on MDIO", sizeof status_text);
        chip_err = "NET NO PHY";
        return;
    }

    if (netInit() != NO_ERROR) {
        lv_strlcpy(status_text, "Ethernet: stack init failed", sizeof status_text);
        chip_err = "NET ERR";
        return;
    }
    ifc = netGetDefaultInterface();

    // Seed for TCP sequence numbers etc.: unique ID plus the SysTick count.
    eth_hw_uid(uid);
    uint8_t seed[16];
    memcpy(seed, uid, 12);
    uint32_t t = lv_tick_get() ^ (*(volatile uint32_t *)0xE000E018UL << 8);
    memcpy(seed + 12, &t, 4);
    netSeedRand(seed, sizeof seed);

    // Locally administered MAC from the chip's unique ID.
    mac.b[0] = 0x02;
    mac.b[1] = uid[0] ^ uid[6];
    mac.b[2] = uid[1] ^ uid[7];
    mac.b[3] = uid[2] ^ uid[8] ^ uid[11];
    mac.b[4] = uid[3] ^ uid[9];
    mac.b[5] = uid[4] ^ uid[10] ^ uid[5];

    netSetInterfaceName(ifc, "eth0");
    netSetHostname(ifc, PMC_HOSTNAME);
    netSetMacAddr(ifc, &mac);
    netSetDriver(ifc, &stm32f4xxEthDriver);
    netSetPhyDriver(ifc, &lan8720PhyDriver);
    netSetPhyAddr(ifc, (uint8_t)phy_addr);
    if (netConfigInterface(ifc) != NO_ERROR) {
        lv_strlcpy(status_text, "Ethernet: interface config failed", sizeof status_text);
        chip_err = "NET ERR";
        return;
    }

    DhcpClientSettings ds;
    dhcpClientGetDefaultSettings(&ds);
    ds.interface = ifc;
    ds.rapidCommit = FALSE;
    ds.timeout = PMC_DHCP_TIMEOUT_MS;
    ds.timeoutEvent = dhcp_timeout;
    dhcpClientInit(&dhcp, &ds);
    dhcpClientStart(&dhcp);

    listener = socketOpen(SOCKET_TYPE_STREAM, SOCKET_IP_PROTO_TCP);
    if (listener == NULL ||
        socketBind(listener, &IP_ADDR_ANY, PMC_TCP_PORT) != NO_ERROR ||
        socketListen(listener, 1) != NO_ERROR) {
        lv_strlcpy(status_text, "Ethernet: TCP listen failed", sizeof status_text);
        chip_err = "NET ERR";
        return;
    }
    socketSetTimeout(listener, 0);
    up = true;
}

static void drop_client(void)
{
    if (client) {
        socketClose(client);
        client = NULL;
    }
}

void ethlink_task(void)
{
    if (!up)
        return;
    netTask();

    // New connection replaces any existing one.
    Socket *s = socketAccept(listener, NULL, NULL);
    if (s) {
        drop_client();
        client = s;
        socketSetTimeout(client, 0);
    }
    if (client) {
        TcpState st = tcpGetState(client);
        if (st != TCP_STATE_ESTABLISHED && st != TCP_STATE_SYN_RECEIVED)
            drop_client();
    }

    Ipv4Addr a = IPV4_UNSPECIFIED_ADDR;
    ipv4GetHostAddr(ifc, &a);
    if (a != IPV4_UNSPECIFIED_ADDR)
        ipv4AddrToString(a, ip_text);
    else
        ip_text[0] = '\0';
}

void ethlink_printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    size_t written;

    if (!client)
        return;
    va_start(args, fmt);
    int len = lv_vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    if (len <= 0)
        return;
    if (len >= (int)sizeof buf)
        len = sizeof buf - 1;
    // Zero timeout: whatever does not fit in the TX buffer is dropped.
    socketSend(client, buf, (size_t)len, &written, 0);
}

bool ethlink_getline(char *buf, size_t n)
{
    static char line[96];
    static size_t len;
    char rx[32];
    size_t got = 0;

    if (!client)
        return false;
    // One small read per call; the main loop runs every ~5 ms.
    if (socketReceive(client, rx, sizeof rx, &got, 0) != NO_ERROR || got == 0)
        return false;
    for (size_t i = 0; i < got; i++) {
        char c = rx[i];
        if (c == '\r')
            continue;
        if (c == '\n') {
            if (len == 0)
                continue;
            line[len] = '\0';
            lv_strlcpy(buf, line, n);
            len = 0;
            // Characters after the newline in this read are dropped; the
            // PC tools send one command and wait for the reply.
            return true;
        }
        if (len < sizeof line - 1)
            line[len++] = c;
    }
    return false;
}

const char *ethlink_chip_text(void)
{
    if (!up)
        return chip_err;
    if (!netGetLinkState(ifc))
        return "NO CABLE";
    return ip_text[0] ? ip_text : "DHCP...";
}

const char *ethlink_ip(void)
{
    return ip_text;
}

bool ethlink_link_up(void)
{
    return up && netGetLinkState(ifc);
}

bool ethlink_client_connected(void)
{
    return client != NULL;
}

const char *ethlink_status(void)
{
    if (!up)
        return status_text;
    if (!netGetLinkState(ifc))
        lv_snprintf(status_text, sizeof status_text, "Ethernet: no cable link (PHY %d)", phy_addr);
    else if (!ip_text[0])
        lv_snprintf(status_text, sizeof status_text, "Ethernet: link up, waiting for DHCP");
    else
        lv_snprintf(status_text, sizeof status_text, "Ethernet: %s%s port %d, %s",
                    ip_text, fallback ? " (no DHCP, fallback)" : "", PMC_TCP_PORT,
                    client ? "client connected" : "no client");
    return status_text;
}

#endif // PMC_LINK_ETHERNET
