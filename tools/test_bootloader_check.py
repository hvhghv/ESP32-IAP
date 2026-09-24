#!/usr/bin/env python3
"""Verify the bootloader field-consistency check logic.

Mirrors iap_image_check_bootloader() from main/iap_image.c and checks the
byte3 bitfield interpretation against real ESP-IDF bootloader headers.

Purpose: this check is a DIAGNOSTIC aid for identifying which build
configuration produced the bootloader currently in flash. Field
inconsistency does NOT prevent booting (verified experimentally).

Key fact (esp_app_format.h):
    uint8_t spi_speed : 4;   // LOW nibble  (bits 0-3)
    uint8_t spi_size  : 4;   // HIGH nibble (bits 4-7)
  => byte3 = (spi_size << 4) | spi_speed
"""
import sys
import os

# ESP image header enums
SPI_MODE = {"qio": 0, "qout": 1, "dio": 2, "dout": 3}
SPI_SIZE = {"1MB": 0, "2MB": 1, "4MB": 2, "8MB": 3,
            "16MB": 4, "32MB": 5, "64MB": 6, "128MB": 7}
SPI_SPEED = {"40m": 0, "26m": 1, "20m": 2, "80m": 0xF}

# This firmware's config (from sdkconfig)
FW = {"mode": "dio", "size": "4MB", "speed": "80m", "chip_id": 13}


def parse_byte3(byte3):
    """Byte3 -> (spi_speed, spi_size) per esp_app_format.h bitfields."""
    return (byte3 & 0x0F), ((byte3 >> 4) & 0x0F)


def check(hdr, fw):
    """Mirror of iap_image_check_bootloader().

    hdr: dict with magic/segment_count/spi_mode/byte3/chip_id
    Returns (present, compatible, detail)
    """
    if hdr["magic"] != 0xE9:
        return False, False, f"no valid bootloader (magic=0x{hdr['magic']:02X})"

    if hdr["chip_id"] != fw["chip_id"]:
        return True, False, (f"chip_id mismatch: bootloader={hdr['chip_id']}, "
                             f"firmware={fw['chip_id']}")

    if hdr["spi_mode"] != SPI_MODE[fw["mode"]]:
        return True, False, (f"spi_mode mismatch: bootloader={hdr['spi_mode']}, "
                             f"firmware={SPI_MODE[fw['mode']]}")

    speed, size = parse_byte3(hdr["byte3"])
    if size != SPI_SIZE[fw["size"]]:
        return True, False, (f"flash size mismatch: bootloader={size}, "
                             f"firmware={SPI_SIZE[fw['size']]}")

    return True, True, (f"compatible (chip_id={hdr['chip_id']}, "
                        f"mode={hdr['spi_mode']}, size={size})")


def main():
    ok = True

    # 1. Real bootloader from the build (byte3 = 0x20)
    real = {"magic": 0xE9, "segment_count": 3, "spi_mode": 2,
            "byte3": 0x20, "chip_id": 13}
    speed, size = parse_byte3(0x20)
    print(f"real bootloader byte3=0x20 -> speed={speed}, size={size}")
    if (speed, size) != (0, 2):
        print("  FAIL: expected speed=0 (40M), size=2 (4MB)")
        ok = False
    else:
        print("  OK: low nibble=speed=0, high nibble=size=2")

    present, compat, detail = check(real, FW)
    print(f"  check -> present={present}, compatible={compat}")
    print(f"    {detail}")
    if not (present and compat):
        print("  FAIL: real bootloader should be compatible")
        ok = False

    # 2. Wrong flash size (8MB) -> detect
    bad_size = dict(real, byte3=(SPI_SIZE["8MB"] << 4) | SPI_SPEED["40m"])
    present, compat, detail = check(bad_size, FW)
    print(f"\nwrong size (8MB) byte3=0x{bad_size['byte3']:02X} "
          f"-> compatible={compat}")
    print(f"    {detail}")
    if compat or "size mismatch" not in detail:
        print("  FAIL: should detect size mismatch")
        ok = False
    else:
        print("  OK: size mismatch detected")

    # 3. Wrong chip id -> detect
    bad_chip = dict(real, chip_id=9)   # esp32s3
    present, compat, detail = check(bad_chip, FW)
    print(f"\nwrong chip (id=9) -> compatible={compat}")
    print(f"    {detail}")
    if compat or "chip_id mismatch" not in detail:
        print("  FAIL: should detect chip mismatch")
        ok = False
    else:
        print("  OK: chip mismatch detected")

    # 4. Wrong spi mode -> detect
    bad_mode = dict(real, spi_mode=0)  # qio
    present, compat, detail = check(bad_mode, FW)
    print(f"\nwrong mode (qio=0) -> compatible={compat}")
    print(f"    {detail}")
    if compat or "spi_mode mismatch" not in detail:
        print("  FAIL: should detect mode mismatch")
        ok = False
    else:
        print("  OK: mode mismatch detected")

    # 5. No bootloader (erased flash) -> detect
    erased = dict(real, magic=0xFF)
    present, compat, detail = check(erased, FW)
    print(f"\nerased flash (magic=0xFF) -> present={present}, compatible={compat}")
    print(f"    {detail}")
    if present or compat:
        print("  FAIL: should report no bootloader")
        ok = False
    else:
        print("  OK: missing bootloader detected")

    # 6. spi_speed must NOT be compared (false-positive guard)
    #    bootloader has speed=0 (40M) but firmware config says 80m
    present, compat, detail = check(real, FW)
    if compat:
        print("\nspi_speed NOT compared -> no false positive (correct)")
    else:
        print("\nFAIL: spi_speed comparison caused false positive")
        ok = False

    print("\nRESULT: " + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
