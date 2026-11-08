#!/usr/bin/env python3
"""Check the images of a sysbuild build against the flash partitions they are written to.

Run by every hardware build (app/sysbuild.cmake, the check_image_sizes target), or by hand:

    python3 scripts/check_image_sizes.py <build dir>
    python3 scripts/check_image_sizes.py --mcuboot <build>/mcuboot --app <build>/app

It fails (exit status 1) when

* MCUboot's flashed binary (zephyr.bin) does not fit boot_partition, which nothing else checks: the
  linker of the MCUboot image does not know the partition;
* the application's signed image (zephyr.signed.bin) does not fit slot0_partition minus the swap
  trailer (CONFIG_MCUBOOT_UPDATE_FOOTER_SIZE, which sysbuild takes from MCUboot's configuration);
  an image above that size is built but MCUboot cannot swap it;
* an image with PSRAM (CONFIG_ESP_SPIRAM) places more in external RAM than CONFIG_ESP_SPIRAM_SIZE:
  from the start of ext_dram_seg to _ext_ram_end in the linker map (<kernel bin name>.map). That
  covers the copied text and rodata, the static data (the display buffers, the LVGL pool) and the
  SPIRAM heap; the linker's own overflow check does not count the copies. A missing map or symbol
  fails the check;
* slot0_partition and slot1_partition differ in size, or MCUboot and the application disagree on any
  partition (the two are built from different devicetrees: app/boards/partitions/ is meant to be
  the one table both include).

The margins are printed whether or not the check passes. The sizes come from the images' build
directories (zephyr/zephyr.dts, zephyr/.config and, for PSRAM, the linker map), nothing else is needed.
"""

import argparse
import re
import sys
from pathlib import Path

# <label>: partition@<address> { ... reg = < offset size >; ... }, in the generated zephyr.dts.
PARTITION_RE = re.compile(r"^\s*(\w+_partition):\s*partition@[0-9a-fA-F]+\s*\{(.*?)^\s*\};", re.MULTILINE | re.DOTALL)
REG_RE = re.compile(r"\breg\s*=\s*<\s*(0x[0-9a-fA-F]+|\d+)\s+(0x[0-9a-fA-F]+|\d+)\s*>")
EXT_DRAM_RE = re.compile(r"^ext_dram_seg\s+(0x[0-9a-fA-F]+)\s+", re.MULTILINE)
EXT_RAM_END_RE = re.compile(r"^\s+(0x[0-9a-fA-F]+)\s+_ext_ram_end\s*=", re.MULTILINE)
CONFIG_RE = re.compile(r"^(CONFIG_\w+)=(.*)$", re.MULTILINE)


def read_partitions(build_dir: Path) -> dict[str, tuple[int, int]]:
    """Maps each *_partition label of the image's devicetree to (offset, size)."""
    dts = build_dir / "zephyr" / "zephyr.dts"
    if not dts.is_file():
        raise SystemExit(f"{dts} not found: is {build_dir} a configured Zephyr build directory?")

    partitions = {}
    for label, body in PARTITION_RE.findall(dts.read_text()):
        reg = REG_RE.search(body)
        if reg:
            partitions[label] = (int(reg.group(1), 0), int(reg.group(2), 0))
    return partitions


def read_config(build_dir: Path) -> dict[str, str]:
    """The image's Kconfig values, from .config and then the settings sysbuild passed in."""
    config: dict[str, str] = {}
    # .config.sysbuild comes second so that it fills in what the final .config leaves out: the
    # footer size is not a symbol at all in an image without the image manager.
    for name in (".config", ".config.sysbuild"):
        path = build_dir / "zephyr" / name
        if path.is_file():
            for key, value in CONFIG_RE.findall(path.read_text()):
                config.setdefault(key, value.strip().strip('"'))
    return config


def read_psram_use(map_file: Path) -> tuple[int, int]:
    """(ext_dram_seg origin, _ext_ram_end) from the image's linker map; ValueError names what is missing."""
    if not map_file.is_file():
        raise ValueError(f"{map_file} not found: the linker map is missing")
    text = map_file.read_text(errors="replace")
    origin = EXT_DRAM_RE.search(text)
    end = EXT_RAM_END_RE.search(text)
    missing = [name for name, found in (("ext_dram_seg", origin), ("_ext_ram_end", end)) if not found]
    if missing:
        raise ValueError(f"{map_file} has no {' or '.join(missing)} symbol")
    return int(origin.group(1), 0), int(end.group(1), 0)


def fmt(value: int) -> str:
    return f"{value:,} B"


def line(name: str, file: str, size: int, limit_text: str, limit: int) -> str:
    free = limit - size
    return (
        f"  {name:<8} {file:<22} {fmt(size):>14}   {limit_text}: {fmt(limit)}   "
        f"margin {'-' if free < 0 else ''}{fmt(abs(free))} ({free / limit:.1%})"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    parser.add_argument(
        "build_dir", nargs="?", type=Path, help="sysbuild build directory (holds mcuboot/ and the application)"
    )
    parser.add_argument("--mcuboot", type=Path, help="MCUboot's build directory (default: <build dir>/mcuboot)")
    parser.add_argument("--app", type=Path, help="the application's build directory (default: <build dir>/app)")
    args = parser.parse_args()

    mcuboot_dir = args.mcuboot or (args.build_dir / "mcuboot" if args.build_dir else None)
    app_dir = args.app or (args.build_dir / "app" if args.build_dir else None)
    if mcuboot_dir is None or app_dir is None:
        parser.error("give the build directory, or both --mcuboot and --app")

    failures: list[str] = []

    mcuboot_parts = read_partitions(mcuboot_dir)
    app_parts = read_partitions(app_dir)
    mcuboot_config = read_config(mcuboot_dir)
    app_config = read_config(app_dir)

    for label in ("boot_partition", "slot0_partition", "slot1_partition"):
        for who, parts in (("MCUboot", mcuboot_parts), ("application", app_parts)):
            if label not in parts:
                failures.append(f"{who}'s devicetree has no {label}")
    if failures:
        print("\n".join(["Image size check FAILED:", *(f"  {f}" for f in failures)]), file=sys.stderr)
        return 1

    # The two images are built from different devicetrees (MCUboot gets the table through
    # mcuboot_EXTRA_DTC_OVERLAY_FILE), so a stray difference would pass every size check and still
    # make MCUboot look for the application in the wrong place.
    for label in sorted(set(mcuboot_parts) | set(app_parts)):
        if mcuboot_parts.get(label) != app_parts.get(label):
            failures.append(
                f"{label} differs: MCUboot {mcuboot_parts.get(label)}, application {app_parts.get(label)} "
                "(offset, size); both must build from app/boards/partitions/"
            )

    boot_size = mcuboot_parts["boot_partition"][1]
    slot0_size = app_parts["slot0_partition"][1]
    slot1_size = app_parts["slot1_partition"][1]
    if slot0_size != slot1_size:
        failures.append(f"slot0_partition ({fmt(slot0_size)}) and slot1_partition ({fmt(slot1_size)}) differ in size")

    footer = int(app_config.get("CONFIG_MCUBOOT_UPDATE_FOOTER_SIZE", "0"), 0)
    rom_end_offset = int(app_config.get("CONFIG_ROM_END_OFFSET", "0"), 0)
    if footer == 0:
        failures.append(
            "CONFIG_MCUBOOT_UPDATE_FOOTER_SIZE is 0 or missing: sysbuild did not pass MCUboot's footer size on"
        )
    app_limit = slot0_size - footer

    mcuboot_bin = mcuboot_dir / "zephyr" / f"{mcuboot_config.get('CONFIG_KERNEL_BIN_NAME', 'zephyr')}.bin"
    app_bin = app_dir / "zephyr" / f"{app_config.get('CONFIG_KERNEL_BIN_NAME', 'zephyr')}.signed.bin"
    sizes = {}
    for who, path in (("MCUboot", mcuboot_bin), ("application", app_bin)):
        if path.is_file():
            sizes[who] = path.stat().st_size
        else:
            failures.append(f"{who}: {path} not found (is the image built and, for the application, signed?)")

    print("Image sizes against the flash partitions:")
    if "MCUboot" in sizes:
        print(line("MCUboot", mcuboot_bin.name, sizes["MCUboot"], "boot_partition", boot_size))
        if sizes["MCUboot"] > boot_size:
            failures.append(
                f"MCUboot ({fmt(sizes['MCUboot'])}) does not fit boot_partition ({fmt(boot_size)}); "
                "trim app/sysbuild/mcuboot.conf, or enlarge the partition (a USB-only change)"
            )
    if "application" in sizes:
        print(
            line(
                "app", app_bin.name, sizes["application"], f"slot0 {fmt(slot0_size)} - footer {fmt(footer)}", app_limit
            )
        )
        if sizes["application"] > app_limit:
            failures.append(
                f"the signed application ({fmt(sizes['application'])}) does not fit slot0_partition minus the swap "
                f"trailer ({fmt(app_limit)}): MCUboot could not swap it"
            )
    if app_config.get("CONFIG_ESP_SPIRAM") != "y":
        print("  PSRAM check skipped: the application has no CONFIG_ESP_SPIRAM")
    else:
        psram_size = int(app_config.get("CONFIG_ESP_SPIRAM_SIZE", "0"), 0)
        map_file = app_dir / "zephyr" / f"{app_config.get('CONFIG_KERNEL_BIN_NAME', 'zephyr')}.map"
        try:
            psram = read_psram_use(map_file)
        except ValueError as error:
            failures.append(f"PSRAM check impossible: {error}")
            psram = None
        if psram is not None and psram_size == 0:
            failures.append("CONFIG_ESP_SPIRAM is set but CONFIG_ESP_SPIRAM_SIZE is 0 or missing")
        elif psram is not None:
            psram_used = psram[1] - psram[0]
            print(line("PSRAM", map_file.name, psram_used, "CONFIG_ESP_SPIRAM_SIZE", psram_size))
            if psram_used > psram_size:
                failures.append(
                    f"the static data in PSRAM ({fmt(psram_used)}, up to _ext_ram_end) exceeds "
                    f"CONFIG_ESP_SPIRAM_SIZE ({fmt(psram_size)}): shrink the image or the LVGL pool"
                )
    print(f"  (CONFIG_ROM_END_OFFSET {rom_end_offset:#x}, CONFIG_MCUBOOT_UPDATE_FOOTER_SIZE {footer:#x})")

    if failures:
        sys.stdout.flush()
        print("Image size check FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
