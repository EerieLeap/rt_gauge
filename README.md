# Eerie Leap Round Gauge

This project is the firmware implementation for a round gauge display device, designed to be used as part of the [EerieLeap](https://github.com/pavllick/EerieLeapRT) sensor monitoring system. Built with Zephyr RTOS, this firmware provides the necessary functionality for the gauge display to integrate seamlessly with the EerieLeap ecosystem.

## Getting Started

Development environment is based on [Docker](https://www.docker.com/). Use `example.docker-compose.yml` as an example. Rename to `docker-compose.yml` to use it.

### Hardware Access From Container

By default, the container will be run with `privileged` mode enabled and `pid` mode set to `host`, to allow access to devices connected to the host. Device have to be connected to the host before running the container.

If using Docker for Windows with Docker using WSL2 engine, make sure to attach device to WSL container. It can be done with help of [usbipd-win](https://github.com/dorssel/usbipd-win) tool. Example:

```shell
usbipd attach --wsl --busid <busid>
```

Where `<busid>` is the bus id of the device that can be found with `usbipd list` command.

Connected device should be visible in the container as `/dev/ttyACM0`. Test presence of the device in the container with `ls /dev/ttyACM0` command.

### Building

For VS Code build setup examples, refer to `.vscode_example/tasks.json`. Alternatively, you can build the application using the following command:

```shell
west build -b $BOARD app
```

where `$BOARD` is the target board.

Currently, the following boards are supported:

#### Simulated boards:
- `native_sim`
- `qemu_cortex_a9`

#### SoC boards:
- `esp32s3_touch_amoled_1_75/esp32s3/procpu` (Waveshare ESP32-S3-Touch-AMOLED-1.75)
- `esp32p4_wifi6_touch_lcd_5/esp32p4/hpcore` (Waveshare ESP32-P4-WIFI6-Touch-LCD-5)

**Hardware builds always use `--sysbuild`.** MCUboot sits in front of the application on both SoC boards: the flash holds
two image slots, the application is linked for the first and signed, and only MCUboot starts it. A plain `west build` of
a SoC board (without `--sysbuild`) builds an ESP simple-boot image instead, linked for and flashed at 0x0: it boots, but it
overwrites MCUboot and ignores the slot layout, so no update can follow. To get MCUboot back, flash a sysbuild build with
`west flash --erase` (see below). A sysbuild directory holds the two images in `mcuboot/` and `app/`
(the application's ELF is `build/app/zephyr/zephyr.elf`, the signed image `build/app/zephyr/zephyr.signed.bin`).
Simulated boards build without it (sysbuild on them builds only the application, since only the Waveshare boards select
MCUboot), so do not make `--sysbuild` a default with `west config build.sysbuild true`.

```shell
west build -p auto -b esp32s3_touch_amoled_1_75/esp32s3/procpu --sysbuild ./app
west build -p auto -b esp32p4_wifi6_touch_lcd_5/esp32p4/hpcore --sysbuild ./app
```

The bench build (shell, SMP over the console, stack reports; see [tools/README.md](tools/README.md)) adds `app/bench.conf`
to the application image only:

```shell
west build -d build_bench -p always -b esp32s3_touch_amoled_1_75/esp32s3/procpu --sysbuild ./app -- -DEXTRA_CONF_FILE=bench.conf
```

(`-Dapp_EXTRA_CONF_FILE=bench.conf` addresses the application image by name and does the same. Do not give MCUboot a
`-Dmcuboot_EXTRA_CONF_FILE`: it replaces `app/sysbuild/mcuboot.conf`.)

#### Flash layout, signing and size checks

Each board's partition table is one file, `app/boards/partitions/<board>.dtsi`: two 4 MB image slots (`slot0` runs,
`slot1` takes the update and then holds the previous image as the backup), LittleFS for the unit's files, and the NVS
partition with the settings and the BLE bonds. The application's overlay includes it and `app/sysbuild.cmake` hands the
same file to MCUboot, so the two cannot disagree. MCUboot swaps the slots in "swap using move" mode and verifies an
ECDSA P-256 signature before every swap (`app/sysbuild.conf`). `app/sysbuild/mcuboot.conf` switches off the display,
MSPI, input and the other board drivers that MCUboot does not need, which it must fit into 64 KB (S3) or 56 KB (P4).

Every sysbuild build ends with `scripts/check_image_sizes.py`, which fails the build when MCUboot outgrows its boot
partition or the signed application outgrows `slot0` minus the swap trailer, and prints both margins. It also compares
the two images' partition tables. Run it by hand on a build directory with `scripts/check_image_sizes.py build`.

Images are signed with the development key, `modules/eerie_leap_rt_core/keys/dev-ec-p256.pem`. It is public and protects
nothing: it only keeps a bench unit from accepting images that were not built for this project. Release images are built
by `.github/workflows/release.yml` with the release key, which lives in a GitHub Actions secret and never in the
repository. A unit trusts the key its MCUboot was built with, so a unit flashed with release artifacts accepts release
images only; changing the key, the mode or the partition table takes a USB flash. To sign a local build with another key,
pass an absolute path, quoted because it is a Kconfig string: `-- '-DSB_CONFIG_BOOT_SIGNATURE_KEY_FILE="/path/to/key.pem"'`.

### Running and Debugging in a Simulator

Running and debugging in a simulator can be more convenient and time-efficient. To run the compiled application in a simulator, use:

```shell
west build -t run
```

For VS Code debugging setup examples, refer to `.vscode_example/launch.json`.

### Flashing

Once the application is built, flash it to the target board with:

```shell
west flash
```

For a sysbuild directory this writes MCUboot and then the application into `slot0` (`--domain mcuboot` or `--domain app`
writes one of them). esptool erases only the sectors it writes, so the swap state MCUboot keeps at the end of `slot0`
survives a plain `west flash`. If a freshly flashed image is swapped back or treated as an update, erase the chip first, as
in the next section.

#### Moving a unit from simple boot to MCUboot

Units built before MCUboot ran the application straight from the ROM bootloader, with the file system at another
offset. Moving a unit to the new layout is a one-time USB flash that erases the file system, so back up first and restore
afterwards. Updates after that go over the air.

1. Back up every configuration and the uploaded assets with the bench client (`tools/smp_can_client.py`; on CAN, with
   `--target`, or on the console of a bench build, with `--serial`):

   ```shell
   python3 tools/smp_can_client.py --serial /dev/ttyACM0 config-list
   python3 tools/smp_can_client.py --serial /dev/ttyACM0 config-read <type> backup/<type>.cbor    # for every type listed
   python3 tools/smp_can_client.py --serial /dev/ttyACM0 assets ls images                         # and animations
   python3 tools/smp_can_client.py --serial /dev/ttyACM0 assets get images <name> backup/<name>   # for every file
   ```

2. Erase the chip and flash the new layout. The old file system's blocks sit inside the new `slot` and `intfs0`
   partitions, and a leftover block must not be taken for a file system or an image. `--erase` runs once, before the
   first image, and then MCUboot and the application are written:

   ```shell
   west flash --erase
   ```

   The erase also removes the BLE bonds, so the configurator app pairs again.

3. Restore: `config-write <type> backup/<type>.cbor` for every type, and `assets put <kind> <name> <file>` for the files.
   The gauge rewrites its demo screens and demo assets on every boot while the test scaffolding in
   `UiController::Initialize()` is in place, so its UI configuration may need no restore.

### Board-Specific Commands

<details>
<summary>
    Native Simulator
    <a href="https://docs.zephyrproject.org/latest/boards/native/native_sim/doc/index.html">(Reference Docs)</a>
</summary>
<br>

**Build:**  
```shell
west build -p auto -b native_sim ./app
```

</details>


<details>
<summary>
    QEMU Cortex A9
    <a href="https://docs.zephyrproject.org/latest/boards/qemu/cortex_a9/doc/index.html">(Reference Docs)</a>
</summary>
<br>

**Build:**  
```shell
west build -p auto -b qemu_cortex_a9 ./app
```

</details>


<details>
<summary>
    Waveshare ESP32S3 Touch AMOLED 1.75
    <a href="https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75">(Reference Docs)</a>
</summary>
<br>

**Build:**  
```shell
west build -p auto -b esp32s3_touch_amoled_1_75/esp32s3/procpu --sysbuild ./app
```

**Serial Monitor:**  
```shell
west espressif monitor
```

**Debugging**

Debugging works to some extent <a href="https://github.com/Marus/cortex-debug">Cortex-Debug</a> extension for <a href="https://marketplace.visualstudio.com/items?itemName=marus25.cortex-debug">VS Code</a> can be used for that purpose, config example set up for Docker container can be found in [.vscode_example/launch.json](.vscode_example/launch.json).

For manual GDB run use ether `west debug` or run OpenOCD in one terminal:
```shell
/home/ubuntu/zephyr/workspace/utilities/openocd-esp32/bin/openocd \
-f /home/ubuntu/zephyr/workspace/utilities/openocd-esp32/share/openocd/scripts/board/esp32s3-builtin.cfg \
-c "set ESP32_ONLYCPU 1; set ESP_FLASH_SIZE 0; set ESP_RTOS Zephyr" \
-c "init; halt; esp appimage_offset 0x20000" \
-c "esp32s3.cpu0 configure -rtos Zephyr" \
-c "init" \
-c "reset init"
```

And GDB in another:
```shell
/home/ubuntu/zephyr-sdk-0.17.4/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-gdb \
-ex 'target extended-remote :3333' \
-ex 'symbol-file build/app/zephyr/zephyr.elf' \
-ex 'mon reset halt'   -ex 'maintenance flush register-cache' \
-ex 'break main' \
-ex 'continue'
```

</details>

<details>
<summary>
    Waveshare ESP32-P4-WIFI6-Touch-LCD-5
    <a href="https://www.waveshare.com/wiki/ESP32-P4-WIFI6-Touch-LCD-5">(Reference Docs)</a>
</summary>
<br>

**Build:**  
```shell
west build -p auto -b esp32p4_wifi6_touch_lcd_5/esp32p4/hpcore --sysbuild ./app
```

**Serial Monitor:**  
```shell
west espressif monitor
```

</details>

## Development

### Widget Appearance

See [Widget appearance](app/docs/ui/WIDGET_APPEARANCE.md) for RGBA colors,
whole-widget opacity, activity/visibility compatibility, live-binding examples,
and the reproducible native simulator gallery.

### Bench tools

`tools/smp_can_client.py` reads and writes a unit's stored configurations over SMP, and lists, downloads,
uploads and deletes its asset files (images, animations, scripts, logs), on CAN or on the console of a unit
built with `app/bench.conf` (shell, SMP over the console, stack reports). See [tools/README.md](tools/README.md).

### Formatting

C and C++ sources follow [`.clang-format`](.clang-format), a copy of rt_core's, written for clang-format 23.1.1
(`pip install clang-format==23.1.1`, already in the dev container; older majors reject the configuration).
[`.clang-format-ignore`](.clang-format-ignore) lists what is never formatted: the zcbor codecs, LVGL font and image
converter output, and the Zephyr drivers, boards and bindings, which keep Zephyr's style.

```shell
scripts/format.sh                     # list the files that need formatting
scripts/format.sh --fix               # reformat them in place
scripts/format.sh --changed [BASE]    # only files changed since the merge base with BASE (default origin/main)
scripts/format.sh PATH...             # only these files or directories
git clang-format                      # only the changed lines of staged files
```

The script exits 0 when clean, 1 when files need formatting and 2 on a setup error, such as a clang-format of another
major. To keep a hand-aligned block as written, wrap it in `// clang-format off` … `// clang-format on` with a comment
saying why. CI fails on unformatted code and when `.clang-format` differs from rt_core's.

`git config blame.ignoreRevsFile .git-blame-ignore-revs` hides the reformat commit from `git blame`; GitHub reads the
file by itself.

### Linting

[`.clang-tidy`](.clang-tidy), a copy of rt_core's, is written for clang-tidy 23 (`clang-tidy-23` from LLVM's apt
repository, already in the dev container); [`tests/.clang-tidy`](tests/.clang-tidy) relaxes what ztest's macros make
wrong. clang-tidy needs `native_sim` compile databases: the app's in `build/tidy`, the test suites' in a twister output
directory.

```shell
scripts/tidy.sh --build                                  # build ./app for native_sim into build/tidy, lint app/src
west twister -p native_sim --build-only -j 4 -O twister-out-tidy -T ./tests
scripts/tidy.sh -p build/tidy -p twister-out-tidy        # lint app/src and tests/ (each -p adds a database)
scripts/tidy.sh --changed [BASE] -p build/tidy           # only sources changed since the merge base with BASE
scripts/tidy.sh --fix PATH...                            # apply clang-tidy's fixes, then review git diff
```

The script exits 0 without warnings, 1 with warnings and 2 on a setup error. Generated sources and rt_core's files are
left out. Waive a finding only with a named check and a reason, `// NOLINT(check-name): reason` (also
`NOLINTNEXTLINE`, `NOLINTBEGIN`/`NOLINTEND`), never a bare `NOLINT` or a wildcard. CI fails on any warning.

### Required tools

#### CBOR

CMake uses [zcbor](https://github.com/NordicSemiconductor/zcbor) to generate CBOR helpers from schemas and expects to find it in the system path. To install zcbor:

```shell
pip install zcbor
```

To generate ZCBOR classes use helpers defined in `app/CMakeLists.txt`, new classes will be placed in `app/src/configuration/generated/` directory.
