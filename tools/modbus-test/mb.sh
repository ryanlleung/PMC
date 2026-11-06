#!/bin/bash
# Modbus TCP client for Git Bash (or any bash with /dev/tcp): no Python, no
# installs. Uses only bash, printf, dd, od, awk and timeout.
#   mb.sh <ip> [port]            read the whole map and print it decoded
#   mb.sh <ip> [port] target <mbar>   write the pressure target
#   mb.sh <ip> [port] valve <pct>     move the valve (MANUAL only)
#   mb.sh <ip> [port] auto | manual | stop | reset | ok
#                                     the command coils 0-3
#   mb.sh <ip> [port] pid <set> <kp> <ti> <kd>
#                                     one PID set's gains (map version 2)
#   mb.sh <ip> [port] sched <off|pv> <b1> <b2> <h>
#                                     the gain schedule: boundaries in mbar,
#                                     hysteresis in %
#   mb.sh <ip> [port] writetest  write target 4.8 and the AUTO coil, then an
#                                unsupported code and a read past the map
#   mb.sh <ip> [port] raw <pdu>  send one PDU in hex (e.g. 03 00 0a 00 02)
#                                and print the reply PDU in hex
# Register map: docs/modbus-register-map.md.
IP=${1:?usage: mb.sh <ip> [port] [target <mbar> | valve <pct> | auto | manual | stop | reset | ok | pid <set> <kp> <ti> <kd> | sched <off|pv> <b1> <b2> <h> | writetest | raw <hex bytes>]}
shift
PORT=502
if [[ $1 =~ ^[0-9]+$ ]]; then PORT=$1; shift; fi
CMD=${1:-map}
shift

exec 3<>"/dev/tcp/$IP/$PORT" || { echo "cannot connect to $IP:$PORT"; exit 1; }
TID=0

# Exactly n bytes from the socket as hex pairs, or nothing after 3 s.
readn() { timeout 3 dd bs=1 count="$1" <&3 2>/dev/null | od -An -v -tx1 | tr -s ' \n' ' '; }

# req <hex bytes of the PDU>: prints the reply PDU as hex bytes.
req() {
    local pdu=($*) n=$(( $# + 1 )) out="" b
    TID=$(( (TID + 1) & 0xffff ))
    for b in $(printf '%02x %02x 00 00 %02x %02x 01' $((TID >> 8)) $((TID & 255)) $((n >> 8)) $((n & 255))) "${pdu[@]}"; do
        out+="\\x$b"
    done
    printf "$out" >&3
    local hdr=($(readn 7))
    if [ ${#hdr[@]} -ne 7 ]; then echo "no reply"; return 1; fi
    local len=$(( 0x${hdr[4]} * 256 + 0x${hdr[5]} - 1 ))
    readn "$len"
    echo
}

# Exception check: prints "exception N" and returns 1 for an error reply.
ex() {
    local r=($1)
    if (( 0x${r[0]} & 0x80 )); then echo "exception $(( 0x${r[1]} ))"; return 1; fi
    return 0
}

# f32 <number>: the IEEE 754 single as four hex bytes, high byte first.
f32() {
    awk -v x="$1" 'BEGIN {
        s = 0; if (x < 0) { s = 1; x = -x }
        if (x == 0) { u = 0 } else {
            e = 0; while (x >= 2) { x /= 2; e++ } while (x < 1) { x *= 2; e-- }
            m = int((x - 1) * 8388608 + 0.5); if (m == 8388608) { m = 0; e++ }
            u = (e + 127) * 8388608 + m
        }
        u += s * 2147483648
        printf "%02x %02x %02x %02x", int(u / 16777216), int(u / 65536) % 256, int(u / 256) % 256, u % 256 }'
}

# One write: prints the reply and "accepted" or the exception.
wr() {
    local r
    r=$(req "$@") || exit 1
    printf '%s  ' "$r"; ex "$r" && echo "accepted"
}

case $CMD in
target)
    wr 10 00 0c 00 02 04 $(f32 "${1:?target <mbar>}")
    ;;
valve)
    wr 10 00 10 00 02 04 $(f32 "${1:?valve <pct>}")
    ;;
auto)   wr 05 00 00 ff 00 ;;
manual) wr 05 00 00 00 00 ;;
stop)   wr 05 00 01 ff 00 ;;
reset)  wr 05 00 02 ff 00 ;;
ok)     wr 05 00 03 ff 00 ;;
pid)
    # Registers 33 + 6 x (set - 1): KP, TI, KD as floats.
    n=${1:?pid <set> <kp> <ti> <kd>}
    [[ $n =~ ^[123]$ ]] || { echo "set 1, 2 or 3"; exit 1; }
    wr 10 00 $(printf %02x $(( 33 + 6 * (n - 1) ))) 00 06 0c $(f32 "${2:?kp}") $(f32 "${3:?ti}") $(f32 "${4:?kd}")
    ;;
sched)
    # Registers 27-32: type (0 off, 1 setpoint, 2 pressure), B1, B2, H.
    case ${1:?sched <off|pv> <b1> <b2> <h>} in off) t=0 ;; pv) t=2 ;; *) echo "off or pv"; exit 1 ;; esac
    h=${4:?h}
    wr 10 00 1b 00 06 0c 00 $(printf %02x $t) $(f32 "${2:?b1}") $(f32 "${3:?b2}") $(printf '%02x %02x' $(( h >> 8 )) $(( h & 255 )))
    ;;
raw)
    req "$@"
    ;;
writetest)
    # Target 4.8 mbar (0x4099999a) to registers 12-13, then AUTO coil on.
    r=$(req 10 00 0c 00 02 04 40 99 99 9a); printf 'write target 4.8: %s  ' "$r"; ex "$r" && echo "accepted"
    r=$(req 05 00 00 ff 00);                printf 'AUTO coil on:     %s  ' "$r"; ex "$r" && echo "accepted"
    r=$(req 04 00 00 00 01);                printf 'FC4 (unsupported): %s  ' "$r"; ex "$r" && echo "accepted"
    r=$(req 03 00 2e 00 0a);                printf 'read past the map: %s  ' "$r"; ex "$r" && echo "accepted"
    ;;
map)
    regs=$(req 03 00 00 00 33) || exit 1
    ex "$regs" || exit 1
    coils=$(req 01 00 00 00 04)
    echo "$regs" | awk -v coils="$coils" '
    function hex(h,   i, v) { v = 0; h = tolower(h)
        for (i = 1; i <= length(h); i++) v = v * 16 + index("0123456789abcdef", substr(h, i, 1)) - 1
        return v }
    function reg(i) { return hex($(3 + 2 * i) $(4 + 2 * i)) }
    function f32(i,   u, s, e, m, v) { u = reg(i) * 65536 + reg(i + 1)
        s = int(u / 2147483648); e = int(u / 8388608) % 256; m = u % 8388608
        if (e == 255) return "nan"
        v = (e == 0) ? m * 2 ^ -149 : (1 + m / 8388608) * 2 ^ (e - 127)
        return sprintf("%.3f", s ? -v : v) }
    {
        split("NOT HOMED,HOMING,MANUAL,AUTO", mode, ",")
        split("reading OK,position set,homed,moving,closed switch pressed,fault,warning,target set,remote writes on,motor supply on", sn, ",")
        printf "map version %d, heartbeat %d, mode %s\n", reg(0), reg(1), (reg(2) < 4 ? mode[reg(2) + 1] : reg(2))
        s = ""; for (b = 0; b < 10; b++) if (int(reg(3) / 2 ^ b) % 2) s = s (s ? ", " : "") sn[b + 1]
        printf "status: %s\n", s ? s : "none"
        printf "regulation %d, faults 0x%04x, warnings 0x%04x, fault count %d, events %d\n", reg(4), reg(5), reg(6), reg(7), reg(8)
        printf "pressure      %s mbar\n", f32(10)
        printf "target        %s mbar\n", f32(12)
        printf "valve         %s %%\n", f32(14)
        printf "valve target  %s %%\n", f32(16)
        printf "range         %s mbar\n", f32(18)
        printf "excitation    %s V\n", f32(20)
        printf "temperature   %s C\n", f32(22)
        printf "uptime        %d s\n", reg(24) * 65536 + reg(25)
        split("off,1,on (pressure)", sch, ",")
        printf "PID table     %s, B1 %s mbar, B2 %s mbar, hysteresis %d %%, set in use %d\n", (reg(27) < 3 ? sch[reg(27) + 1] : reg(27)), f32(28), f32(30), reg(32), reg(26)
        for (k = 0; k < 3; k++)
            printf "set %d        KP %s, TI %s, KD %s\n", k + 1, f32(33 + 6 * k), f32(35 + 6 * k), f32(37 + 6 * k)
        split(coils, c, " ")
        printf "coils 0-3 (AUTO, STOP, RESET, OK): %d%d%d%d\n", hex(c[3]) % 2, int(hex(c[3]) / 2) % 2, int(hex(c[3]) / 4) % 2, int(hex(c[3]) / 8) % 2
    }'
    ;;
*)
    echo "unknown command $CMD"; exit 1
    ;;
esac
exec 3<&-
