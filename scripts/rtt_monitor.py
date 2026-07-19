#!/usr/bin/env python3
"""
rtt_monitor.py — Connect to J-Link and stream RTT output to console.

Usage:
    ./scripts/rtt_monitor.py
    ./scripts/rtt_monitor.py --elf app/build/app/zephyr/zephyr.elf --snr 260114597

Requires pylink-square (bundled with NCS toolchain).
Run from your nRF Connect terminal (environment must have pylink on PATH).
"""

import os
import sys
import time
import argparse
from pathlib import Path

import pylink


def find_rtt_address(elf_path: Path) -> int:
    """Extract _SEGGER_RTT symbol address from the ELF map or directly."""
    # Try the .map file first (easier to parse)
    map_path = elf_path.with_suffix(".map")
    if map_path.exists():
        for line in map_path.read_text().splitlines():
            if "_SEGGER_RTT" in line and not line.startswith(" "):
                parts = line.strip().split()
                if parts:
                    try:
                        return int(parts[0], 16)
                    except ValueError:
                        pass

    # Fallback: try reading from the ELF directly
    try:
        from elftools.elf.elffile import ELFFile
    except ImportError:
        print("ERROR: pyelftools not found. Install with: pip install pyelftools", file=sys.stderr)
        sys.exit(1)

    with open(elf_path, "rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab") or elf.get_section_by_name(".dynsym")
        if not symtab:
            print("ERROR: No symbol table found in ELF", file=sys.stderr)
            sys.exit(1)
        for sym in symtab.iter_symbols():
            if sym.name == "_SEGGER_RTT":
                return sym.entry.st_value

    print(f"ERROR: _SEGGER_RTT not found in {elf_path}", file=sys.stderr)
    sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="Stream RTT output from J-Link to console")
    parser.add_argument("--elf", default=None,
                        help="Path to zephyr.elf (default: app/build/app/zephyr/zephyr.elf)")
    parser.add_argument("--snr", default=None,
                        help="J-Link serial number (default: auto-detect)")
    parser.add_argument("--device", default="nRF9160_xxAA",
                        help="Device name (default: nRF9160_xxAA)")
    parser.add_argument("--channel", default=0, type=int,
                        help="RTT channel to read (default: 0)")
    args = parser.parse_args()

    # Resolve ELF path
    if args.elf:
        elf_path = Path(args.elf)
    else:
        script_dir = Path(__file__).resolve().parent
        project_dir = script_dir.parent
        elf_path = project_dir / "app" / "build" / "app" / "zephyr" / "zephyr.elf"

    if not elf_path.exists():
        print(f"ERROR: ELF not found at {elf_path}. Build first or specify --elf.", file=sys.stderr)
        sys.exit(1)

    # Get RTT control block address
    rtt_addr = find_rtt_address(elf_path)
    print(f"  _SEGGER_RTT @ 0x{rtt_addr:08X}", file=sys.stderr)

    # Connect to J-Link
    jlink = pylink.JLink()

    try:
        if args.snr:
            jlink.open(serial_no=int(args.snr))
        else:
            jlink.open()

        print(f"  Connected to J-Link (S/N: {jlink.serial_number})", file=sys.stderr)

        # Set interface and speed
        jlink.set_tif(pylink.enums.JLinkInterfaces.SWD)
        # jlink.set_speed(args.if_speed)

        # Retry connecting to target (device may still be resetting from flash)
        print(f"  Connecting to {args.device}...", file=sys.stderr)
        for attempt in range(5):
            try:
                jlink.connect(args.device, verbose=False)
                # Verify connection
                _ = jlink.core_id()
                break
            except pylink.errors.JLinkException as e:
                if attempt < 4:
                    print(f"  Retrying connection ({attempt + 1}/5)...", file=sys.stderr)
                    time.sleep(1)
                else:
                    print(f"  Failed to connect: {e}", file=sys.stderr)
                    sys.exit(1)

        print(f"  Target connected: core ID 0x{jlink.core_id():08X}", file=sys.stderr)
        print(f"  Halting target for RTT setup...", file=sys.stderr)
        jlink.halt()
        time.sleep(0.2)

        # Start RTT
        jlink.rtt_start(rtt_addr)

        print(f"  RTT started, control block @ 0x{rtt_addr:08X}", file=sys.stderr)
        print(f"  ----- RTT output -----", file=sys.stderr)

        # Resume target execution
        jlink.restart()
        time.sleep(0.2)

        # Read loop
        _LEVEL_COLORS = {
            "<err>": "\033[31m",   # red
            "<wrn>": "\033[33m",   # yellow
            "<inf>": "\033[36m",   # cyan
            "<dbg>": "\033[90m",   # gray
        }
        _RESET = "\033[0m"

        buf = bytearray()
        while True:
            try:
                data = jlink.rtt_read(args.channel, 1024)
                if data:
                    buf.extend(bytes(data))
                    # Flush on newlines
                    while b"\n" in buf:
                        idx = buf.index(b"\n")
                        line = bytes(buf[:idx]).decode("utf-8", errors="replace")
                        del buf[: idx + 1]
                        # Colorize log level tags
                        for tag, color in _LEVEL_COLORS.items():
                            if tag in line:
                                line = line.replace(tag, f"{color}{tag}{_RESET}")
                                break
                        print(line)
                        sys.stdout.flush()
                else:
                    time.sleep(0.05)
            except KeyboardInterrupt:
                print(file=sys.stderr)
                print("  RTT monitor stopped.", file=sys.stderr)
                break

    finally:
        try:
            jlink.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
