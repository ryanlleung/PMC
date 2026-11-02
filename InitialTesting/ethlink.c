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
#include "link.h"
#include "modbus.h"
#include "cfg.h"

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
static char mask_text[16] = "";
static char mac_text[18] = "";
static char status_text[112] = "Ethernet: not started";
static const char *chip_err = "NET";   // short fault for the header
static bool was_linked;
static bool new_client;

// Line assembly for client input; reset when a new client connects.
static link_line_t line;
static char rx[64];
static size_t rx_len, rx_pos;

// Modbus TCP on PMC_MODBUS_PORT (0.12.0): up to PMC_MODBUS_CLIENTS at once,
// a new one past that replaces the one idle longest. Requests are answered
// in the same main-loop pass; modbus.c does the protocol, mbmap.c the map.
typedef struct {
    Socket *s;
    uint8_t buf[MODBUS_ADU_MAX];
    size_t len;
    uint32_t last_ms;
    char ip[16];
} mb_client_t;
static Socket *mb_listener;
static mb_client_t mb[PMC_MODBUS_CLIENTS];

// Called by the MAC driver (overrides the library's weak default).
void stm32f4xxEthInitGpio(NetInterface *interface)
{
    (void)interface;
    eth_hw_init_pins();
}

static void dhcp_timeout(DhcpClientContext *context, NetInterface *interface)
{
    Ipv4Addr a, m;
    char ip[16], mask[16];
    dhcpClientStop(context);
    cfg_net_boot(ip, mask);   // saved (0.12.13), else PMC_FALLBACK_IP
    ipv4StringToAddr(ip, &a);
    ipv4StringToAddr(mask, &m);
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
    lv_snprintf(mac_text, sizeof mac_text, "%02X:%02X:%02X:%02X:%02X:%02X",
                mac.b[0], mac.b[1], mac.b[2], mac.b[3], mac.b[4], mac.b[5]);

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

    // Modbus is an extra: if it cannot listen the text link still runs.
    mb_listener = socketOpen(SOCKET_TYPE_STREAM, SOCKET_IP_PROTO_TCP);
    if (mb_listener &&
        (socketBind(mb_listener, &IP_ADDR_ANY, PMC_MODBUS_PORT) != NO_ERROR ||
         socketListen(mb_listener, PMC_MODBUS_CLIENTS) != NO_ERROR)) {
        socketClose(mb_listener);
        mb_listener = NULL;
    }
    if (mb_listener)
        socketSetTimeout(mb_listener, 0);
    up = true;
}

// Everything written to s has been sent and acknowledged.
static bool tx_drained(const Socket *s)
{
    return s->sndUser == 0 && s->sndNxt == s->sndUna;
}

static void mb_drop(mb_client_t *c)
{
    if (c->s) {
        socketClose(c->s);
        c->s = NULL;
    }
    c->len = 0;
}

static void mb_task(void)
{
    if (!mb_listener)
        return;

    IpAddr from;
    uint16_t port;
    Socket *s = socketAccept(mb_listener, &from, &port);
    if (s) {
        mb_client_t *c = &mb[0];
        for (int i = 0; i < PMC_MODBUS_CLIENTS; i++) {
            if (!mb[i].s) { c = &mb[i]; break; }
            if (lv_tick_elaps(mb[i].last_ms) > lv_tick_elaps(c->last_ms))
                c = &mb[i];
        }
        mb_drop(c);
        c->s = s;
        c->last_ms = lv_tick_get();
        socketSetTimeout(s, 0);
        ipAddrToString(&from, c->ip);
    }

    for (int i = 0; i < PMC_MODBUS_CLIENTS; i++) {
        mb_client_t *c = &mb[i];
        if (!c->s)
            continue;
        // CLOSE_WAIT: closed by the client, its last requests still answered.
        TcpState st = tcpGetState(c->s);
        if (st != TCP_STATE_ESTABLISHED && st != TCP_STATE_SYN_RECEIVED && st != TCP_STATE_CLOSE_WAIT) {
            mb_drop(c);
            continue;
        }
        // A client that sends half a request and stops is dropped.
        if (c->len && lv_tick_elaps(c->last_ms) > PMC_MODBUS_IDLE_MS) {
            mb_drop(c);
            continue;
        }
        size_t got = 0;
        if (socketReceive(c->s, c->buf + c->len, sizeof c->buf - c->len, &got, 0) != NO_ERROR || got == 0) {
            if (st == TCP_STATE_CLOSE_WAIT && tx_drained(c->s))
                mb_drop(c);   // closed, all read and the replies acknowledged
            continue;
        }
        c->len += got;
        c->last_ms = lv_tick_get();
        // Every whole request in the buffer, in order.
        for (;;) {
            size_t n = modbus_frame_len(c->buf, c->len);
            if (n == (size_t)-1) {
                mb_drop(c);
                break;
            }
            if (n == 0)
                break;
            uint8_t out[MODBUS_ADU_MAX];
            size_t written;
            mbmap_set_peer(c->ip);
            size_t r = modbus_handle(c->buf, n, out);
            if (r)
                socketSend(c->s, out, r, &written, 0);
            memmove(c->buf, c->buf + n, c->len - n);
            c->len -= n;
        }
    }
}

int ethlink_modbus_clients(void)
{
    int n = 0;
    for (int i = 0; i < PMC_MODBUS_CLIENTS; i++)
        n += mb[i].s != NULL;
    return n;
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

    // After the DHCP fallback, a cable unplug and replug (e.g. moving from a
    // laptop to the site switch) tries DHCP again.
    bool linked = netGetLinkState(ifc);
    if (linked && !was_linked && fallback) {
        ipv4SetHostAddr(ifc, IPV4_UNSPECIFIED_ADDR);
        fallback = false;
        dhcpClientStart(&dhcp);
    }
    was_linked = linked;

    // New connection replaces any existing one.
    Socket *s = socketAccept(listener, NULL, NULL);
    if (s) {
        drop_client();
        client = s;
        socketSetTimeout(client, 0);
        line.len = rx_len = rx_pos = 0;
        line.over = false;   // an over-long line from the last client is not this one's
        new_client = true;
    }
    if (client) {
        // CLOSE_WAIT: the client has closed its side, but what it sent
        // before (STOP and close, `nc -N`) may still be unread; the socket
        // goes once ethlink_getline has read it all.
        TcpState st = tcpGetState(client);
        if (st != TCP_STATE_ESTABLISHED && st != TCP_STATE_SYN_RECEIVED && st != TCP_STATE_CLOSE_WAIT)
            drop_client();
    }

    mb_task();

    Ipv4Addr a = IPV4_UNSPECIFIED_ADDR;
    ipv4GetHostAddr(ifc, &a);
    if (a != IPV4_UNSPECIFIED_ADDR) {
        Ipv4Addr m = IPV4_UNSPECIFIED_ADDR;
        ipv4GetSubnetMask(ifc, &m);
        ipv4AddrToString(a, ip_text);
        ipv4AddrToString(m, mask_text);
    } else {
        ip_text[0] = mask_text[0] = '\0';
    }
}

size_t ethlink_tx_room(void)
{
    if (!client)
        return (size_t)-1;
    // Buffered and not yet sent, plus sent and not yet acknowledged.
    size_t used = (size_t)client->sndUser + (size_t)(client->sndNxt - client->sndUna);
    return used < client->txBufferSize ? client->txBufferSize - used : 0;
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
    if (!client)
        return false;

    for (;;) {
        if (rx_pos >= rx_len) {
            size_t got = 0;
            rx_pos = rx_len = 0;
            if (socketReceive(client, rx, sizeof rx, &got, 0) != NO_ERROR || got == 0) {
                // Closed by the client and all read. A last line with no
                // line ending is not run (it may be cut short: VALV SET 5 of
                // 50). The socket goes once the replies have been sent and
                // acknowledged, so a reply is not lost to the close.
                if (tcpGetState(client) == TCP_STATE_CLOSE_WAIT && tx_drained(client))
                    drop_client();
                return false;
            }
            rx_len = got;
        }
        // Bytes after a newline stay in rx for the next call.
        while (rx_pos < rx_len)
            if (link_line_feed(&line, rx[rx_pos++], buf, n))
                return true;
    }
}

bool ethlink_take_new_client(void)
{
    bool v = new_client;
    new_client = false;
    return v;
}

const char *ethlink_chip_text(void)
{
    if (!up)
        return chip_err;
    if (!netGetLinkState(ifc))
        return "NO CABLE";
    return ip_text[0] ? ip_text : "DHCP...";
}

const char *ethlink_mac(void)
{
    return mac_text;
}

const char *ethlink_mask(void) { return mask_text; }
bool ethlink_fallback(void) { return fallback; }

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
        lv_snprintf(status_text, sizeof status_text, "Ethernet: %s%s port %d, %s; Modbus %d: %d",
                    ip_text, fallback ? " (no DHCP, fallback)" : "", PMC_TCP_PORT,
                    client ? "client connected" : "no client", PMC_MODBUS_PORT,
                    ethlink_modbus_clients());
    return status_text;
}

#endif // PMC_LINK_ETHERNET
