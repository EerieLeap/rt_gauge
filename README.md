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
- `qemu_cortex_m3`

#### SoC boards:
- `esp32s3_devkitc_procpu`

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
    QEMU Cortex M3
    <a href="https://docs.zephyrproject.org/latest/boards/qemu/cortex_m3/doc/index.html">(Reference Docs)</a>
</summary>
<br>

**Build:**  
```shell
west build -p auto -b qemu_cortex_m3 ./app
```

</details>


<details>
<summary>
    Waveshare ESP32S3 Touch AMOLED 1.75
    <a href="https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-1.75">(Reference Docs)</a>
</summary>
<br>

**Build with Bootloader:**  
```shell
west build -p auto -b esp32s3_devkitc/esp32s3/procpu --sysbuild ./app
```

**Simple Build:**  
```shell
west build -p auto -b esp32s3_devkitc/esp32s3/procpu ./app
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
-c "init; halt; esp appimage_offset 0" \
-c "esp32s3.cpu0 configure -rtos Zephyr" \
-c "init" \
-c "reset init"
```

And GDB in another:
```shell
/home/ubuntu/zephyr-sdk-0.17.4/xtensa-espressif_esp32s3_zephyr-elf/bin/xtensa-espressif_esp32s3_zephyr-elf-gdb \
-ex 'target extended-remote :3333' \
-ex 'symbol-file build/zephyr/zephyr.elf' \
-ex 'mon reset halt'   -ex 'maintenance flush register-cache' \
-ex 'break main' \
-ex 'continue'
```

</details>

## Development

### Widget Appearance

See [Widget appearance](app/docs/ui/WIDGET_APPEARANCE.md) for RGBA colors,
whole-widget opacity, activity/visibility compatibility, live-binding examples,
and the reproducible native simulator gallery.

### Required tools

#### CBOR

CMake uses [zcbor](https://github.com/NordicSemiconductor/zcbor) to generate CBOR helpers from schemas and expects to find it in the system path. To install zcbor:

```shell
pip install zcbor
```

To generate ZCBOR classes use helpers defined in `app/CMakeLists.txt`, new classes will be placed in `app/src/configuration/generated/` directory.
