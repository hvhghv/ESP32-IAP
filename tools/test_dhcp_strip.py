#!/usr/bin/env python3
"""Verify the DHCP option-strip logic fix.

Reproduces the bug: after stripping ROUTER/DNS, the new end pointer must be
the exact end of the remaining options (NOT the original end).
"""
import sys

PAD = 0
SUBNET = 1
ROUTER = 3
DNS = 6
MTU = 26
BROADCAST = 28
MSGTYPE = 53
SERVERID = 54
END = 255


def tlv(t, v):
    return bytes([t, len(v)]) + bytes(v)


# Simulate the OFFER options (as seen in the device log, after magic cookie)
opts = b""
opts += tlv(MSGTYPE, [2])           # 53 OFFER
opts += tlv(SUBNET, [255, 255, 255, 0])
opts += tlv(51, [0, 0, 0x1c, 0x20])
opts += tlv(SERVERID, [192, 168, 4, 1])
opts += tlv(ROUTER, [192, 168, 4, 1])    # <- remove
opts += tlv(DNS, [192, 168, 4, 1])       # <- remove
opts += tlv(BROADCAST, [192, 168, 4, 255])
opts += tlv(MTU, [0x05, 0xdc])
opts += tlv(31, [0])
opts += tlv(43, [1, 4, 0, 0, 0, 2])

orig_len = len(opts)
# Physical buffer (options array is 312 bytes total incl. cookie)
PHYS = 312
buf = bytearray(opts) + bytes(PHYS - len(opts))
end = len(opts)          # current end offset


def strip_option(buf, end, opt_id):
    """Mirror of iap_dhcp_strip_option (returns new end)."""
    p = 0
    removed = 0
    while p < end:
        t = buf[p]
        if t == END:
            break
        if t == PAD:
            p += 1
            continue
        if p + 1 >= end:
            break
        ln = buf[p + 1]
        nxt = p + 2 + ln
        if nxt > end:
            break
        if t == opt_id:
            remain = end - nxt
            if remain > 0:
                buf[p:p + remain] = buf[nxt:end]
            total = nxt - p
            buf[end - total:end] = bytes([PAD]) * total
            end -= total
            removed += 1
            continue
        p = nxt
    return end, removed


def options_len_buggy(buf, opts_start, end):
    """The OLD buggy recompute: walks through PADs to the physical end."""
    p = opts_start
    while p < end:
        t = buf[p]
        if t == END:
            return p - opts_start
        if t == PAD:
            p += 1
            continue
        if p + 1 >= end:
            break
        ln = buf[p + 1]
        nxt = p + 2 + ln
        if nxt > end:
            break
        p = nxt
    return p - opts_start


print(f"original options: {orig_len} bytes")

# --- OLD (buggy) behaviour: strip then recompute with options_len ---
buf_old = bytearray(buf)
end_old, n_old = strip_option(buf_old, end, ROUTER)
end_old, n_old2 = strip_option(buf_old, end_old, DNS)
# BUG: recompute using ORIGINAL end -> walks through PADs
new_end_buggy = options_len_buggy(buf_old, 0, end)
print(f"[OLD] stripped {n_old + n_old2} opts, strip end={end_old}, "
      f"recomputed end={new_end_buggy}  <-- WRONG (should be {end_old})")

# --- NEW (fixed) behaviour: strip returns the end directly ---
buf_new = bytearray(buf)
end_new, n1 = strip_option(buf_new, end, ROUTER)
end_new, n2 = strip_option(buf_new, end_new, DNS)
print(f"[NEW] stripped {n1 + n2} opts, returned end={end_new}")

ok = True
if new_end_buggy != end_old:
    print("  -> confirmed: old recompute produced a WRONG (too large) end")
else:
    print("  -> old recompute happened to match (unexpected)")
    ok = False

if end_new == end_old:
    print("  -> fix OK: new end matches the true stripped length")
else:
    print("  -> FIX FAILED")
    ok = False

# Verify the remaining options are exactly what we expect
expect = (tlv(MSGTYPE, [2]) + tlv(SUBNET, [255, 255, 255, 0]) +
          tlv(51, [0, 0, 0x1c, 0x20]) + tlv(SERVERID, [192, 168, 4, 1]) +
          tlv(BROADCAST, [192, 168, 4, 255]) + tlv(MTU, [0x05, 0xdc]) +
          tlv(31, [0]) + tlv(43, [1, 4, 0, 0, 0, 2]))
if bytes(buf_new[:end_new]) == expect:
    print(f"  -> remaining options correct ({end_new} bytes): "
          f"no ROUTER(3), no DNS(6)")
else:
    print("  -> remaining options MISMATCH")
    print("     got: " + buf_new[:end_new].hex())
    print("     exp: " + expect.hex())
    ok = False

# Verify tail after new end is zeroed
if all(b == 0 for b in buf_new[end_new:PHYS]):
    print(f"  -> tail zeroed ({PHYS - end_new} bytes)")
else:
    print("  -> tail NOT zeroed")
    ok = False

print("\nRESULT: " + ("PASS" if ok else "FAIL"))
sys.exit(0 if ok else 1)
