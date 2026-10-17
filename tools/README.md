# Tools

| File | Purpose |
| --- | --- |
| `smp_can_client.py` | An SMP client for a unit, over CAN or over its console. Reads and writes stored configurations as YAML or JSON, and lists, downloads, uploads and deletes its asset files. |
| `LVGLImage.py` | LVGL's image converter, for the UI's asset files. |
| `update_zephyr.sh` | Updates the west workspace to `west.yml` with the build's patches reverted and applied again. |

## SMP client

`smp_can_client.py` sends SMP requests to one unit and prints the responses. It speaks the `os`,
`device`, `network`, `config` and `assets` groups the units serve (see rt_core's `docs/smp.md`); the `live`,
`calibration` and `files` groups have no command here.

### Requirements

| Needed for | Package |
| --- | --- |
| Every command | `cbor2` |
| CAN | `python-can` |
| Serial | `pyserial` |
| Descriptions (`config-get`, `config-set`, `config-encode`, `config-decode`) | `PyYAML` and `zcbor` 0.9.1 |

A package is imported when a command needs it, so a serial bench needs no `python-can`. zcbor 0.9.1
imports a name that cbor2 6 removed; the client restores it before the import.

### Transports

| Transport | Selected by | Options |
| --- | --- | --- |
| CAN | `--target ID` | `--interface`, `--channel`, `--bitrate`, `--fd`, `--data-bitrate`, `--base`, `--source`, `--share` |
| Serial | `--serial PORT` | `--baud` (115200), `--line-delay` (seconds between the lines of a request, 0), `--show-console` |

`--serial` and `--target` exclude each other. `--timeout` (2 s) and `--retries` apply to both. A failed
request is resent once on CAN and three times on serial, where a log line that lands inside a response
corrupts it.

The serial transport speaks MCUmgr's shell transport on the unit's console: lines that start `0x06 0x09`
(first) or `0x04 0x14` (continuation), the base64 of a 16-bit length, the packet and its CRC16, at most
127 bytes each. The console's log lines and shell echo between the lines are skipped, and a response
that a log line cut is dropped and asked for again. `--show-console` prints that text to stderr, including
the log text that cut a frame line, and the counts of corrupt lines and dropped packets when the port closes.

Only a bench build answers on the console. Without the shell transport the unit does not reply, and the
client reports a timeout that names `app/bench.conf`. See [Bench builds](#bench-builds).

#### Opening a port

The S3's USB-Serial-JTAG emulates the classic auto-reset circuit: the chip is held in reset while RTS is
high and DTR low. Linux raises both lines in one request when it opens the port, and pyserial then
applies DTR before RTS, so the usual presets (both low) pass through exactly that state and reset the
unit. The client presets DTR high and RTS low and lowers DTR after the open: the lines go
(1,1), (1,0), (0,0) and never reach (0,1). A terminal program should not touch the lines either:

```shell
python -m serial.tools.miniterm --rts 0 /dev/ttyACM1 115200
```

Leave DTR alone there. A port that the client holds is exclusive; close the terminal first.

### Commands

| Command | Transport | Action |
| --- | --- | --- |
| `echo TEXT`, `params`, `info`, `devices`, `config-list` | either | One request each. |
| `reset` | either | Restarts the unit (`os` reset); a lost answer is no failure. |
| `config-crc TYPE` | either | Length, CRC32 and applied flag of the stored configuration. |
| `config-read TYPE FILE`, `config-write TYPE FILE` | either | The stored CBOR bytes, unchanged. `TYPE` is a number or a name. |
| `config-get TYPE [FILE]` | either | Reads a configuration and prints it as a description (YAML; a `.json` file name selects JSON). |
| `config-set TYPE FILE` | either | Encodes a description (`-` is standard input) and writes it; the unit validates and applies it. |
| `config-encode TYPE DESCRIPTION OUTPUT`, `config-decode TYPE INPUT [OUTPUT]` | none | The same conversion offline, between a description and a CBOR file. |
| `assets kinds`, `assets ls KIND` | either | The asset kinds the unit serves, and the files of one (see [Assets](#assets)). |
| `assets get KIND NAME [FILE] [--in-flight N] [--resume]` | either | Downloads a file and checks its CRC32. |
| `assets put KIND NAME FILE` | either | Uploads a file under the name NAME. |
| `assets rm KIND NAME` | either | Deletes a file. |
| `shell TEXT [--wait S]` | serial | Types TEXT as a shell command line and prints the console for S seconds (1). An empty TEXT only listens: `shell "" --wait 15` catches a thread analyzer report. |
| `bench [--size N] [--count N]` | either | Echo round trips, for throughput. |

#### Writing a configuration

The write returns once the unit applied the configuration, and applying restarts services that log and
stops logging on every unit (a stored configuration is an update; a refused one is not). The
request that completes a write is therefore sent once, with at least 15 s to answer, and is never resent
blindly: a second one would stage and apply the bytes again, or meet a closed session. When its answer
is lost or corrupted, the client reads the stored length, CRC32 and applied flag (`config-crc`). A match
means the unit stored and applied the bytes, since it stores them as received. The client reads that state
before it writes: when the unit stored the same bytes already, a match proves nothing, so the transfer
starts over instead, and the unit may apply the bytes twice. A transfer that is not confirmed starts over
once, after the unit's 5 s session timeout. Earlier chunks and reads are resent as usual: the unit
answers a repeated chunk with where it expects the next one. A refused configuration fails with the
validator's reason, such as `Debounce is only supported for indicator sensors.`.

### Assets

The `assets` group serves a unit's files by kind: an image or a script is addressed by its kind and a flat
name, never by a path. `KIND` is a name or a number, as in the table; what a unit serves (and with which
access, size cap and extensions) is what `assets kinds` prints, one line per kind:

```
kind 0  images  dir ui_assets/images  access RWD  max 4194304 B  ext .bin  avail yes
kind 4  logs  dir logs  access R-D  max none  ext .mf4  avail yes
```

`access` shows R (download), W (upload) and D (delete) for what the kind allows and `-` for what it does
not (`R-D`: logs can be downloaded and deleted, not uploaded), `max` is the largest upload (`none` for a
kind that takes none) and `avail` says whether the volume is mounted now.

| Number | Name | Served by |
| --- | --- | --- |
| 0 | `images` | the gauge, from `ui_assets/images` |
| 1 | `animations` | the gauge, from `ui_assets/animations` |
| 2 | `sensor_scripts` | a unit with an SD card and sensors, from `scripts/sensors` |
| 3 | `can_scripts` | a unit with an SD card and a CAN bus, from `scripts/can` |
| 4 | `logs` | a unit that logs, from `logs` (download and delete only; the file being written is in use) |

A unit serves the kinds it has the services for, so the gauge lists the first two. `assets ls` prints the
size and name of each file. A name is 1 to 64 characters of `A-Z a-z 0-9 . _ -`, does not start with `.`,
and ends in one of the kind's extensions with at least one character before it. A unit built before the
group answers every `assets` command with `MCUmgr error 8 (the unit has no assets group (older firmware))`.

```shell
python3 tools/smp_can_client.py --channel can0 --target 1 assets kinds
python3 tools/smp_can_client.py --channel can0 --target 1 assets ls logs
python3 tools/smp_can_client.py --channel can0 --target 1 assets get logs log_data_0001.mf4 --resume
python3 tools/smp_can_client.py --serial /dev/ttyACM0 assets put images logo.bin ./logo.bin
python3 tools/smp_can_client.py --serial /dev/ttyACM0 assets rm images logo.bin
```

#### Downloading

`get` writes `FILE` (`NAME` in the current directory by default; a directory gets `NAME` inside it). The file
is only touched once the unit has answered, so a wrong name leaves an existing file alone. The first `READ`
goes alone: its answer carries the size of the file and, in its data length, the length of every chunk but
the last. The client then keeps `--in-flight N` requests (2 by default) outstanding at consecutive offsets
and matches the responses by SMP sequence number, which hides the round trip of each chunk. A request that
gets no answer is sent again under its own sequence number, as many times as `--retries` says; a chunk that
is short without being the last leaves a gap, which is asked for again. The unit's packets are one pool for
requests and responses (`params` shows `buf_count`), so a window of `buf_count` would exhaust it: the client
lowers `--in-flight` to `buf_count - 1` and says so (`--in-flight` takes 1 to 255).

The unit answers the requests one after another, so the last request of a full window waits for the
responses of the others. Each pipelined request is therefore given `--timeout` once per request in flight:
`--in-flight 3` with the default 2 s waits 6 s, which a slow bus needs (at 125 kbit/s and 25 % of the bus, a
1 KB response takes about 0.75 s). The price is that a response that got lost is found, and asked for
again, after that longer time.

The last chunk carries the CRC32 of the whole file. The client computes the CRC32 of the local file (not of
what it received) and compares: on a mismatch it prints both, keeps the file and exits with 1. A mismatch
can also mean that the asset changed during the download. A download that ends in an error keeps what it has.

`--resume` continues a partial `FILE` from its size (a file that does not exist starts at 0). The first
request then asks for the bytes after the local ones; the unit reads the file up to there to seed its CRC32
before it answers, so that request waits at least 30 s (more for a larger `--timeout`). A local file longer
than the asset is refused. The CRC32 check covers the whole file, so a partial file of another version of
the asset is caught at the end: download it again without `--resume`.

At the end `get` prints the size, the CRC32 and the rate. Over CAN the rate is set by the unit that sends
the data, not by the client: its stored `smp_bus_share_percent` (25 % of the bus by default) caps the bus
time its SMP frames use, and the bus does the rest. Raising `--in-flight` does not raise it past that cap.

#### Uploading

`put` sends the file in chunks that fit the unit's packet (`params` `buf_size`, less an allowance for the
request's header, kind, the name, offsets, length, CRC32 and token). The allowance counts the name as at
least 64 characters, so the chunks have one size for any name, and as many as the name has beyond that; over
the console it also keeps 4 bytes for the length and CRC16 of the frame. A request the unit's transport
cannot take is dropped without an answer, so a name that leaves no room for data is refused before anything
is sent. The first chunk carries
the size and the CRC32 of the file (`zlib.crc32`) and is answered with a token, which the later chunks
carry. The unit stages the data in a file no listing shows, checks the CRC32 with the last chunk and then
replaces the asset, so a failed or abandoned upload leaves the old file. Only one upload is open at a time
(`BUSY` for another), and the unit drops one that is idle for 5 s.

Chunks go one after the other. Each is sent again after a timeout, and a lost answer is no problem: the
unit answers a first chunk sent again with its token, a chunk it already holds with `BAD_OFFSET` and the
offset it expects (where the client continues), and the last chunk, for 5 s after the file was stored,
with the same answer again. A file of one chunk that was stored is answered again with its token as well.
An upload that is lost before the last chunk (no answer to a chunk, `BAD_TOKEN`, `BUSY`) starts over once,
after the unit's 5 s timeout. If the last chunk gets no answer, or `BAD_TOKEN`, the outcome is unknown, since
the file may be stored already: the client says so, and `assets ls` or `assets get`
shows which it is. A file in use (`IN_USE`), too large (`TOO_LARGE`), without room (`NO_SPACE`) or of a kind
that takes no uploads (`DENIED`) fails with the unit's reason, and so does a CRC32 that does not match.

`rm` deletes a file. A delete whose answer was lost and that is sent again meets `NOT_FOUND`, which the client
takes for done; a first request that meets it fails.

### Types

| Number | Name | Schema |
| --- | --- | --- |
| 1 | `system` | rt_core `system_config.cddl` |
| 2 | `adc` | rt_core `adc_config.cddl` |
| 3 | `canbus` | rt_core `canbus_config.cddl` |
| 4 | `sensors` | rt_core `sensors_config.cddl` |
| 5 | `logging` | rt_core `logging_config.cddl` |
| 6 | `ui` | the gauge's `ui_config.cddl` |
| 7 | `display` | the gauge's `display_config.cddl` |
| 8 | `logging_control` | rt_core `logging_control_config.cddl` |

The numbers are `ConfigurationService::Type`. A unit registers the types it has; `config-list` shows them.
The schemas are those of this checkout, so a description matches the firmware built from it, not
necessarily the firmware on a unit.

### Descriptions

A description is the configuration with the names the CDDL gives its members. zcbor parses the schema
and validates every encoded item; the client maps the description over the schema's tree, and writes
floats with the width the schema says (`float32` as a single-precision float, which the firmware's
decoders require; zcbor's own YAML conversion writes double precision).

| CDDL | Description |
| --- | --- |
| An array whose members are all named (`[a: uint, ? b: tstr]`) | A mapping, in any order. Every member is required except those marked `?`, which may be left out (or `null`). A name the schema lacks is an error. |
| An array with one unnamed member (`[* T]`, `[0*16 T]`) | A list of T. |
| A map (`{* uint => T}`, `{2*50(float32 => float32)}`) | A mapping; keys are converted to the key type, so JSON's `"0.5"` becomes 0.5. A list of `[key, value]` pairs works too. The order is kept. |
| A choice (`int / tstr / bool`) | The value's own type; the first alternative that fits, in schema order. |
| `uint`, `int` | An integer. A bool is not one. |
| `bool`, `tstr` | `true`/`false`, a string. |
| `float32`, `float` | A number. |

There are no defaults: every member that the schema requires is written, as the firmware's validators
check them. Integers are accepted for float members. In YAML, exponent notation needs a decimal point and a
signed exponent (`1.0e+3`, or just `1000.0`): `1e3` and `1.0e3` are strings. Errors name the member:
`sensors[1].configuration.debounce_ms: missing`. A description file that is not valid YAML or JSON fails
with `error:` and the file name. Bytes the schema does not accept, such as a sensors file in the format
before the debounce fields, fail to decode with an error that names the type.

The values a bench needs:

| Field | Values |
| --- | --- |
| Sensor `type` | `NONE` 0, `PHYSICAL_ANALOG` 1, `VIRTUAL_ANALOG` 2, `PHYSICAL_INDICATOR` 3, `VIRTUAL_INDICATOR` 4, `CANBUS_RAW` 5, `CANBUS_ANALOG` 6, `CANBUS_INDICATOR` 7, `USER_ANALOG` 8, `USER_INDICATOR` 9 |
| Sensor `interpolation_method` | `NONE` 0, `LINEAR` 1, `CUBIC_SPLINE` 2 |
| Sensor `input_mode` | `Level` 0, `Toggle` 1 |
| Sensor `sampling_rate_ms` | -1 for none (a source that updates by itself) |
| Sensor `connection_string` | `bus/frame/signal` for CAN sensors (`0/289/bench_switch`, `0/0x3B0/IgnitionOn`); a sensor's CAN signal must exist on that bus in the CAN configuration |
| CAN `send_interval_ms` | -1 or 0 for a message the unit only receives |
| CAN channel `type` | `CLASSICAL_CAN` 1, `CANFD` 2 |
| Trigger `channel` | `Sensors` 1, `Logging` 2 |
| Trigger `event_type`, `payload_key` (sensors channel) | `DataUpdated` 0; payload `SensorId` 0, `Value` 1 |
| Trigger `target_uid` | CDMP UID of the unit the trigger starts and stops; 0 for every unit that can log (one broadcast) |
| `sensor_id_hash` (logging configuration) | The 32-bit hash of the sensor id, `StringHelpers::GetHash` in rt_core; see its `smp.md` |

An indicator sensor and a trigger that logs while it is on:

```yaml
# sensors
sensors:
- id: bench_switch
  is_enabled: true
  metadata: {name: Bench switch, unit: '', description: Set from the bench}
  configuration:
    type: 7                     # CANBUS_INDICATOR
    sampling_rate_ms: -1
    interpolation_method: 0
    connection_string: 0/289/bench_switch
    script_path: ''
    debounce_ms: 0
    input_mode: 0
```

```yaml
# logging_control
triggers:
- channel: 1                    # Sensors
  event_type: 0                 # DataUpdated
  payload_key: 1                # Value
  has_selector: true
  selector_key: 0               # SensorId
  selector_value: bench_switch
  target_uid: 0                 # CDMP UID of the unit to start and stop; 0 = every unit that can log
```

### Bench builds

`app/bench.conf` (here) and rt_daq's `app/bench.conf` are Kconfig fragments for a bench build, a
development aid that is not for a shipped build:

```shell
west build -d build_bench -b esp32s3_touch_amoled_1_75/esp32s3/procpu ./app -- -DEXTRA_CONF_FILE=bench.conf
```

`EXTRA_CONF_FILE` stays in the CMake cache: build the bench image in a directory of its own, or leave it
with `-p always`, so a later build of the same directory is not a bench build by accident.

| Setting | Why |
| --- | --- |
| `SHELL`, `SHELL_STACK_SIZE=4096` | The Zephyr shell on the console. The gauge has none otherwise. |
| `MCUMGR_TRANSPORT_SHELL`, `BASE64`, `_RX_BUF_COUNT=16`, `_INPUT_TIMEOUT` | SMP over the console. A configuration request is up to 12 lines that wait for the shell thread, which runs at the lowest priority; the default 2 buffers lose lines. The timeout frees the shell after a request cut off mid-frame. |
| `SHELL_BACKEND_SERIAL_INTERRUPT_DRIVEN` | The shell's polling API never hands SMP frames to MCUmgr. The S3 has the interrupt-driven API by default; native_sim needs it set. |
| `SHELL_LOG_BACKEND=n`, `LOG_BACKEND_UART=y`, `LOG_RUNTIME_FILTERING=n` | Logs keep the shipped path: `uart_poll_out`, no lock, the same stack depth on every thread that logs. The shell log backend serializes them on a mutex without priority inheritance and runs deeper on each caller's stack. The shell implies runtime filtering, which would cap every module at INF and drop debug logs. |
| `SHELL_BACKEND_SERIAL_FORCE_TX_BLOCKING_MODE` | Shell output is polled too. Interrupt-driven output waits for a host to drain the USB-Serial-JTAG FIFO and blocks the shell thread, and with it the threads that print, while no host has the port open. |
| `THREAD_ANALYZER_AUTO`, `_AUTO_INTERVAL=10`, `_AUTO_STACK_SIZE=3072`, `_PRINT_THREAD_PRIORITY` | A report of every thread's stack peak every 10 s, through the log; `shell "" --wait 15` catches one. |
| `STACK_SENTINEL` | The S3 has no stack guard; a sentinel stops the unit with the thread's name when a stack overflows. |

rt_daq's fragment also enables `APP_LOGGING_SHELL` and `APP_SENSOR_SHELL` (its `docs/application.md`).

Stack peaks and latencies measured on a bench build use the shipped logging path, plus the shell thread and
the analyzer thread. A thread near its limit can still differ by the shell's own frames; measure it again
without the fragment's shell lines when it matters.

Busy units: the gauge's shell thread competes with its renderer. When requests time out while it renders,
raise `--timeout` first (`--timeout 5`), and `CONFIG_SHELL_THREAD_PRIORITY_OVERRIDE` second.

Measured cost of the fragment on the S3 (`west build` memory report, same sources with and without it;
rt_core `0be6901`; the gauge built from its working tree, with the uncommitted restart monitor of the logging
control work in rt_core and the `CPU_LOAD` and LVGL performance monitor lines of `app/prj.conf` enabled in both
images, so the absolute figures include them):

| Unit | Flash image | `iram0_0_seg` | `dram0_0_seg` of 399108 B | `ext_dram_seg` |
| --- | --- | --- | --- | --- |
| Gauge | 2976964 B to 3116660 B (+139696) | 105784 B to 108092 B (+2308) | 266248 B (66.7 %) to 280728 B (70.3 %) (+14480) | 7774352 B to 7839888 B (+65536) |
| rt_daq | 1564676 B to 1648228 B (+83552) | 79584 B to 81844 B (+2260) | 192368 B (48.2 %) to 206744 B (51.8 %) (+14376) | 3456 KB to 3584 KB (+128 KB) |

The internal RAM the fragment adds is the shell and analyzer stacks (4096 and 3072 B), the 16 receive
buffers (127 B each plus their headers), the shell's state and rings, and the analyzer's tables.

The table is the static image. Both builds set `COMMON_LIBC_MALLOC_ARENA_SIZE=-1`, so the libc heap is the
internal RAM that the image leaves, and it shrinks by about the same amount. Read the internal heap's free
and maximum-used figures on the first boot of each bench image (`SYS_HEAP_RUNTIME_STATS` is on;
`SystemInfo::PrintHeapInfo()` prints them) and compare them with a shipped image's.

### Recipes

Write a unit's sensors and triggers (the same on CAN with `--target` in place of `--serial`):

```shell
python3 tools/smp_can_client.py --serial /dev/ttyACM0 config-set sensors daq_sensors.yaml
python3 tools/smp_can_client.py --serial /dev/ttyACM0 config-set logging_control triggers.yaml
python3 tools/smp_can_client.py --serial /dev/ttyACM0 config-get sensors   # reads back what is stored
```

Drive a logging trigger on rt_daq without wiring an input: store a `CANBUS_INDICATOR` sensor on a signal no
unit sends (`0/289/bench_switch`, with that signal in a message of the CAN configuration), a trigger on it,
and set it from the shell:

```shell
python3 tools/smp_can_client.py --serial /dev/ttyACM0 shell "sensor set bench_switch 1"
python3 tools/smp_can_client.py --serial /dev/ttyACM0 shell "logging status"
```

To let the other unit see the value, use a signal in a message rt_daq sends. The bench units store the CAN
configuration that the `SetupCanbusConfiguration()` test hooks wrote (the calls are commented out now): rt_daq
sends `sensor_0` in frame 100 every 20 ms, and the gauge receives it as `0/100/sensor_0`. Check what a unit
stores with `config-get canbus`; a unit with another configuration needs such a message added with
`config-set canbus` first. The CAN scheduler fills a sent signal with the value of the sensor whose id is the
signal's name, so the rt_daq sensor must be named `sensor_0`. It has none until one is stored, so
`sensor set` writes it and nothing else does.

Capture the stack report:

```shell
python3 tools/smp_can_client.py --serial /dev/ttyACM1 shell "" --wait 15
```

Restore a backed-up configuration file byte for byte:

```shell
python3 tools/smp_can_client.py --serial /dev/ttyACM0 config-write logging logging_config.cbor
```

A sensors file from before the debounce fields is refused (`Malformed CBOR`) and nothing is stored: write
a description with `config-set sensors`, or a file made with `config-encode`, instead.

A unit that does not decode its stored sensors file (a format from before the debounce fields) keeps the
file, does not apply it and runs on defaults; `config-crc` still shows the file. Writing a new one replaces
it. The gauge adds its logging button to the sensors, and the button's trigger to the triggers, at every
boot when they are missing, so a configuration written without them gains them after a restart; on a gauge
whose stored sensors did not decode, the first boot stores a file that holds only the button.
