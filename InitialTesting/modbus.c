#include <string.h>

#include "modbus.h"

/* --------------------------------------------------------------------------
 * Modbus TCP ADU: MBAP header (transaction id 2, protocol id 2 = 0,
 * length 2 = unit id + PDU, unit id 1), then the PDU (function code + data).
 * All fields big-endian. Limits from the Modbus Application Protocol v1.1b3.
 * ------------------------------------------------------------------------ */

#define MBAP_LEN      7
#define PDU_MAX       253
#define FC_READ_COILS 0x01
#define FC_READ_HOLD  0x03
#define FC_WRITE_COIL 0x05
#define FC_WRITE_MULT 0x10
#define COILS_MAX     2000
#define READ_MAX      125
#define WRITE_MAX     123

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

size_t modbus_frame_len(const uint8_t *buf, size_t n)
{
    if (n < MBAP_LEN)
        return 0;
    uint16_t len = get16(buf + 4);
    if (get16(buf + 2) != 0 || len < 2 || len > PDU_MAX + 1)
        return (size_t)-1;
    size_t total = 6u + len;
    return n >= total ? total : 0;
}

// Exception PDU: function code | 0x80, code. Returns the PDU length.
static size_t ex(uint8_t *pdu, uint8_t fc, uint8_t exc)
{
    pdu[0] = fc | 0x80;
    pdu[1] = exc;
    return 2;
}

// PDU in (len bytes, function code first), reply PDU into out.
static size_t pdu(const uint8_t *q, size_t len, uint8_t *r)
{
    uint8_t fc = q[0];
    uint16_t addr, n;

    switch (fc) {
    case FC_READ_COILS:
    case FC_READ_HOLD:
        if (len != 5)
            return ex(r, fc, MODBUS_EX_VALUE);
        addr = get16(q + 1);
        n = get16(q + 3);
        if (n < 1 || n > (fc == FC_READ_COILS ? COILS_MAX : READ_MAX))
            return ex(r, fc, MODBUS_EX_VALUE);
        if ((uint32_t)addr + n > 0x10000u)
            return ex(r, fc, MODBUS_EX_ADDRESS);
        mbmap_begin();
        r[0] = fc;
        if (fc == FC_READ_HOLD) {
            r[1] = (uint8_t)(n * 2);
            for (uint16_t i = 0; i < n; i++) {
                uint16_t v;
                if (!mbmap_read_reg((uint16_t)(addr + i), &v))
                    return ex(r, fc, MODBUS_EX_ADDRESS);
                put16(r + 2 + 2 * i, v);
            }
            return 2u + 2u * n;
        }
        r[1] = (uint8_t)((n + 7) / 8);
        memset(r + 2, 0, r[1]);
        for (uint16_t i = 0; i < n; i++) {
            bool v;
            if (!mbmap_read_coil((uint16_t)(addr + i), &v))
                return ex(r, fc, MODBUS_EX_ADDRESS);
            if (v)
                r[2 + i / 8] |= (uint8_t)(1u << (i % 8));
        }
        return 2u + r[1];

    case FC_WRITE_COIL: {
        if (len != 5)
            return ex(r, fc, MODBUS_EX_VALUE);
        addr = get16(q + 1);
        uint16_t v = get16(q + 3);
        if (v != 0x0000 && v != 0xFF00)
            return ex(r, fc, MODBUS_EX_VALUE);
        bool dummy;
        mbmap_begin();
        if (!mbmap_read_coil(addr, &dummy))
            return ex(r, fc, MODBUS_EX_ADDRESS);
        if (!mbmap_writes_allowed())
            return ex(r, fc, MODBUS_EX_FUNCTION);
        uint8_t e = mbmap_write_coil(addr, v == 0xFF00);
        if (e)
            return ex(r, fc, e);
        memcpy(r, q, 5);   // the reply echoes the request
        return 5;
    }

    case FC_WRITE_MULT: {
        if (len < 6)
            return ex(r, fc, MODBUS_EX_VALUE);
        addr = get16(q + 1);
        n = get16(q + 3);
        uint8_t bytes = q[5];
        if (n < 1 || n > WRITE_MAX || bytes != n * 2 || len != 6u + bytes)
            return ex(r, fc, MODBUS_EX_VALUE);
        if ((uint32_t)addr + n > 0x10000u)
            return ex(r, fc, MODBUS_EX_ADDRESS);
        uint16_t v[WRITE_MAX];
        mbmap_begin();
        for (uint16_t i = 0; i < n; i++) {
            uint16_t old;
            if (!mbmap_read_reg((uint16_t)(addr + i), &old))
                return ex(r, fc, MODBUS_EX_ADDRESS);
            v[i] = get16(q + 6 + 2 * i);
        }
        if (!mbmap_writes_allowed())
            return ex(r, fc, MODBUS_EX_FUNCTION);
        uint8_t e = mbmap_write_regs(addr, n, v);
        if (e)
            return ex(r, fc, e);
        memcpy(r, q, 5);   // function, address, count
        return 5;
    }

    default:
        return ex(r, fc, MODBUS_EX_FUNCTION);
    }
}

size_t modbus_handle(const uint8_t *in, size_t n, uint8_t *out)
{
    size_t total = modbus_frame_len(in, n);
    if (total == 0 || total == (size_t)-1 || total != n) {
        return 0;
    }
    memcpy(out, in, MBAP_LEN);   // transaction id, protocol id, unit id
    size_t plen = pdu(in + MBAP_LEN, n - MBAP_LEN, out + MBAP_LEN);
    put16(out + 4, (uint16_t)(plen + 1));
    return MBAP_LEN + plen;
}

