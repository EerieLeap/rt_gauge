# Image files

An image icon (`IconType` `Image`) draws a file of the `images` asset kind
(`ui_assets/images`). The kind holds three forms, told apart by their extension:

| Extension | What the file is | Its size comes from |
| --- | --- | --- |
| `.lvimg` | LVGL 9's binary image file: a 12-byte header, then the pixels. It describes itself. | the file's header |
| `.bin` | The older form: the pixels alone, in the one colour format `KINDS` reports as `fmt`. | the widget's `IMG_WIDTH` and `IMG_HEIGHT` |
| `.svg` | An SVG document, drawn by an SVG icon (see [SVG icons](SVG_ICONS.md)). | the document |

The extension is the format; the unit never guesses it from the bytes. A raw `.bin` whose first bytes
happen to read as a header is still raw pixels, and an old unit that does not list `.lvimg` refuses the
name instead of drawing a header as pixels.

## The `.lvimg` file

Little-endian, as LVGL's `lv_image_header_t` is on the targets and as `tools/LVGLImage.py --ofmt BIN`
writes it:

| Offset | Bytes | Field | Value |
| --- | --- | --- | --- |
| 0 | 1 | `magic` | `0x19` |
| 1 | 1 | `cf` | the colour format, one the unit lists in `fmts` |
| 2 | 2 | `flags` | 0, or `0x0008` (compressed) where the unit lists `cmps` |
| 4 | 2 | `w` | the width, 1 to 65535 |
| 6 | 2 | `h` | the height, 1 to 65535 |
| 8 | 2 | `stride` | the bytes a row of the colour plane takes, at least the least stride below |
| 10 | 2 | reserved | 0 |
| 12 | | the pixels | |

| `cf` | Format | Least stride | Pixel bytes |
| --- | --- | --- | --- |
| `0x06` | L8 | `w` | `stride × h` |
| `0x12` | RGB565, little-endian | `2w` | `stride × h` |
| `0x14` | RGB565A8 | `2w`, and even | `stride × h`, then the alpha plane: `stride / 2 × h` |
| `0x15` | AL88, `[luminance, alpha]` | `2w` | `stride × h` |

RGB565A8's alpha plane has half the stride (LVGL's rule), so its stride is even. A padded stride, which
`LVGLImage.py --align` writes, is taken as written.

A file is exactly 12 bytes plus its pixel bytes long. One byte more would be another file with another
CRC-32 that draws the same, and the tools compare copies by CRC-32.

Two examples of a header:

- 466 × 466, RGB565: `19 12 00 00 D2 01 D2 01 A4 03 00 00`, a file of 434,324 bytes.
- 15 × 220, RGB565A8: `19 14 00 00 0F 00 DC 00 1E 00 00 00`, a file of 9,912 bytes.

### What the unit takes

The colour formats follow the display's depth (`LV_COLOR_DEPTH`):

| Build | `fmts` | Boards |
| --- | --- | --- |
| 16-bit | `RGB565` (`0x12`), `RGB565A8` (`0x14`) | both gauge boards |
| 8-bit | `L8` (`0x06`), `AL88` (`0x15`) | the native simulator |

A file made for the other depth is refused, not converted.

### Compressed files

Where `KINDS` lists `cmps` (`[2]`, LZ4), a file may be compressed: the header's `flags` is `0x0008` and
the pixels are replaced by this layout:

| Offset | Bytes | Field |
| --- | --- | --- |
| 12 | 4 | the method (`lv_image_compressed_t`): its low four bits are 2 for LZ4 |
| 16 | 4 | the compressed size |
| 20 | 4 | the decompressed size, which must equal the pixel bytes above |
| 24 | | the LZ4 block: no frame, what `LZ4_decompress_safe` reads |

The file is exactly 24 bytes plus the compressed size long. `LVGLImage.py --compress LZ4` writes this
layout. The unit decompresses the file once, when an icon first shows it; LVGL draws the decompressed
pixels, so a compressed file draws as fast as an uncompressed one and only the volume, the link and the
load time see the difference. RLE, LVGL's other method, is not taken.

### The checks

One set of rules judges a file when it is uploaded, when it is listed and when an icon loads it. The first
rule broken is reported, in this order:

1. The file has at least 12 bytes, and the magic is `0x19`.
2. `cf` is one of the build's `fmts`.
3. `flags` is 0, or exactly `0x0008` where the build decompresses. Every other bit describes a buffer, not
   a file, and is refused (premultiplied alpha, the draw-buffer flags, the user bits).
4. The reserved bytes are 0.
5. `w` and `h` are at least 1.
6. The stride is at least the least stride, and even for RGB565A8.
7. The pixel bytes are at most the kind's `max` (`CONFIG_EERIE_LEAP_UI_IMAGE_MAX_SIZE`, 4 MiB). A compressed
   file must fit in RAM once decompressed, so `max` bounds its pixels too.
8. The length is right: 12 plus the pixel bytes, or for a compressed file 24 bytes at least, a method the
   build decompresses, a decompressed size equal to the pixel bytes, and a length of 24 plus the compressed
   size.

## On the wire

The `assets` group (see rt_core's `docs/smp.md`) carries the image facts additively; a client that knows
none of them works as it did.

- **`KINDS`.** The images entry lists `.lvimg` in `ext`, and adds `fmts`, the colour formats a `.lvimg` may
  be in, and `cmps`, the compression methods, where the unit decompresses. A unit whose `ext` lacks
  `.lvimg` is older firmware that takes only `.bin`. `fmt` stays, as the format of the raw `.bin`.
- **`LIST`.** The entry of a `.lvimg` whose header reads carries `img: {w, h, cf, cmp?}`: the width, the
  height, the colour format, and the compression method only when the file is compressed. A client can
  describe a file it has not downloaded. The entry of a `.bin`, an `.svg`, and of a file put on the volume
  some other way whose header does not read, has no `img`.
- **`WRITE`.** The unit judges the header as soon as it holds it, which is the first chunk for any real
  client, before the rest of the file crosses the link. A file that is no image the unit draws is refused
  with `BAD_CONTENT` (18), and the error carries the reason as `msg`. The first chunk's checks run in this
  order: the kind, its write access, the name (`BAD_NAME`), a built-in name, the volume, the size
  (`TOO_LARGE`), the content (`BAD_CONTENT`), `IN_USE`, `BUSY`, `NO_SPACE`. A client that splits the header
  over several chunks is refused on the chunk that completes it, and the staged file is deleted. The last
  chunk is not checked again: its CRC-32 proves the bytes are those the header described.
- **`RENAME`.** A rename keeps the name's extension: `a.bin` cannot become `a.lvimg`, which would turn
  raw pixels into an image file without the header check. It is refused with `BAD_NAME`, for every kind.

### What `BAD_CONTENT` says

| Rule broken | `msg` |
| --- | --- |
| Under 12 bytes | `An image file starts with a 12-byte header; this one is <n> bytes long.` |
| Wrong magic | `Not an LVGL image file: it starts with 0x<byte>, not 0x19.` |
| Colour format | `Colour format 0x<cf> is not one this unit draws (it draws 0x12 RGB565, 0x14 RGB565A8).` |
| Flags | `The header's flags 0x<flags> are not allowed in an image file.` |
| Compressed, not taken | `The image is compressed, and this unit decompresses no images.` |
| Reserved bytes | `The header's reserved bytes are 0x<value>, not 0.` |
| Empty | `An image is at least 1 by 1 pixels; this one is <w> by <h>.` |
| Stride | `A row of <w> pixels in colour format 0x<cf> takes at least <n> bytes; the stride is <s>.` |
| RGB565A8 stride | `The stride of an RGB565A8 image must be even; this one is <s>.` |
| Too many pixels | `The pixels take <n> bytes, more than the <max> this unit loads.` |
| Compressed, short | `A compressed image file starts with 24 bytes of headers; this one is <n> bytes long.` |
| Compression method | `Compression method <m> is not one this unit decompresses.` |
| Decompressed size | `The image decompresses to <n> bytes, not the <p> its pixels take.` |
| Length | `A <w> by <h> image in colour format 0x<cf> with stride <s> is <n> bytes long; this file is <f>.` |
| Compressed length | `The compressed data is <c> bytes, so the file is <n> bytes long; this one is <f>.` |

## How an image icon draws a file

- **A `.lvimg` is drawn at its own size and format.** The icon reads the header, so `IMG_WIDTH` and
  `IMG_HEIGHT` are not read for it: a widget that kept them from the `.bin` days changes nothing.
- **One copy for every widget that shows the file.** The pixels are loaded once into RAM, aligned for the
  draw units, and shared by every icon that names the file. Three widgets on one background hold it once,
  not three times. The copy is released when the last icon lets go of it.
- **A new upload** is picked up the next time an icon is built, not mid-draw: icons that hold the old copy
  keep it until they are built again.
- **A file the unit cannot draw draws nothing**, and the icon logs why once: a missing file, one that fails
  the checks above (a file put on the volume some other way), or one that does not fit in memory. Upload
  it again, or a valid file under the same name, and the next build draws it.
- **A name with another extension** on an image icon (an `.svg`) draws nothing and logs once. Use an SVG icon
  for a document.
- **A `.bin` keeps the older path exactly.** `IMG_WIDTH` and `IMG_HEIGHT` give the size, the file must hold
  at least that many pixels in the `fmt` the kind reports, and each icon keeps a copy of its own.

The icon sizes itself to the image, centred in the widget's box, antialiased, as before. Anchor points and
rotation follow the drawn size.

## What to pick

| The picture | 16-bit unit | 8-bit unit |
| --- | --- | --- |
| Opaque: a background, a photograph, a dial face | `RGB565` | `L8` |
| With transparency: a needle, a logo, a rounded button | `RGB565A8` | `AL88` |

An opaque picture should be RGB565. It has no alpha plane, so it takes a third less room than the same
picture in RGB565A8 (a 466 × 466 background: 434,324 bytes instead of 651,468), on the volume, in RAM and
over the link, and it covers what lies beneath it without the draw path having to find out that every alpha
byte is 255.

Make the file with `tools/LVGLImage.py` (see [tools/README.md](../../../tools/README.md#image-files)):

```shell
python3 tools/LVGLImage.py --ofmt BIN --cf RGB565 -o ./out background.png
mv ./out/background.bin ./out/background.lvimg
python3 tools/smp_can_client.py --serial /dev/ttyACM0 assets put images background.lvimg ./out/background.lvimg
```

The converter writes the header the unit wants, so nothing is cut off; only the name changes. Add
`--compress LZ4` where `assets kinds` lists `cmps`. `assets ls images` shows what the unit read from each
header (`466x466 RGB565`).

## The older `.bin`

A raw `.bin` is the pixels alone. Its size is the widget's `IMG_WIDTH` × `IMG_HEIGHT` and its format the
one `fmt` reports (RGB565A8 on a 16-bit build, AL88 on an 8-bit one), with a byte of alpha for every pixel
even where the picture is opaque. A file shorter than that draws nothing; one of another width draws as
stripes. It is made from the converter's output by cutting off the 12-byte header, and it is uploaded and
listed as before. Existing `.bin` files are not touched or converted: the unit converts nothing, and a
configuration that names one keeps drawing it. A client that knows nothing of `.lvimg` keeps working.

## Downgrading

A unit taken back to firmware from before `.lvimg` does not list the `.lvimg` names (they fail that
firmware's name check), and its icons draw nothing for them. The files stay on the volume and take their
room until firmware that lists them is installed again and they are deleted, or the volume is erased. Where
the room matters, delete the `.lvimg` files before a downgrade, and upload `.bin` files for the older
firmware.
