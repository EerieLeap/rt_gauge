# SVG icons

`IconFactory` creates `Svg` (`IconType` 3), which draws an SVG document stored as an
`images` asset (`ui_assets/images`, `.svg` next to the raw `.bin` images). Use it like
any other icon: in a `BasicIcon` or `BasicArcIcon` widget, or as a dial's needle.

## Properties

| Property | Type | Default | Meaning |
| --- | --- | --- | --- |
| `FILE_PATH` | String | empty | The document, a path relative to the `images` kind's directory |
| `WIDTH_PX` | Integer | 0 | The icon's width in pixels, 0 to follow the document |
| `HEIGHT_PX` | Integer | 0 | The icon's height in pixels, 0 to follow the document |

With neither size set, the icon takes the document's own size: the root's `width` and
`height`, or else its `viewBox`. With one set, the other follows the document's aspect
ratio. With both set, the document is scaled uniformly to fit and centered, and the rest
of the box stays transparent, as SVG's default `preserveAspectRatio` places a `viewBox`.
All three take effect at the next render, as a Lottie widget's do: a document is drawn
on a thread that renders screens, never on the bus worker a binding delivers on.

The document keeps its own colours: like image icons, an SVG icon registers no color
property and the theme does not recolor it. Opacity, visibility, whole-widget animation
and native rotation apply as for every icon.

## What is drawn

LVGL's SVG parser (`CONFIG_LV_USE_SVG`) reads the document: shapes, paths, groups,
transforms, solid colours, gradients and opacity. The root must be `<svg>`. It is not
drawn:

- `<text>`, which LVGL draws with FreeType only. Convert text to paths.
- `<image>` references: no loader is registered, so a document reads no other file.
- SVG animation (`CONFIG_LV_USE_SVG_ANIMATION` is off).

A document LVGL would crash on, or draw without bound, is refused: one with a `<use>`
(LVGL follows it without checking that it refers to anything, or not to itself), with
a fill or stroke that refers to anything but a gradient or a solid colour (LVGL calls
a null pointer), or with dashed strokes (ThorVG generates any number of dashes, however
short). Expand `<use>` and dashes into paths before uploading (Inkscape: Edit > Clone >
Unlink Clone, Path > Stroke to Path).

LVGL's own SVG image decoder is switched off when the renderer starts
(`UiRendererService::Initialize()`): it would parse, on every redraw and without these
checks, any image whose data begins like an SVG document.

## Rendering and limits

The document is parsed and rasterized when the icon renders. ThorVG draws
it into an ARGB8888 buffer in external RAM (4 bytes a pixel), which is then converted from
premultiplied to straight alpha, the only alpha the L8 and AL88 targets blend. A plain
`lv_image` shows the result, so a redraw costs what an image's does, and the parsed
document is freed right after it is drawn. ThorVG's 8-bit blending leaves fully covered
pixels at alpha 254.

A document that cannot be drawn renders nothing and logs why:

- **LVGL heap.** A document's tree, the render list built from it and the shapes it
  queues live on the LVGL heap (`CONFIG_LV_Z_MEM_POOL_SIZE`), at a measured peak of 1.0
  to 1.2 KiB an element plus 2 to 4 bytes a byte of path data. LVGL does not survive
  running out of that heap, so a document is refused unless 1536 bytes an element plus
  8 bytes a byte of it fit the heap's free space: by its size before it is read, by its
  elements, counted by the tokenizer, after. The check needs
  `CONFIG_SYS_HEAP_RUNTIME_STATS`, which `app/prj.conf` enables; without it every
  document is refused. A typical icon of 0.2 to 5 KB and a few dozen elements needs
  well under 100 KiB.
- **Nesting.** LVGL builds and frees the document's tree recursively, and draws each
  nested group with a recursive call that keeps about 500 bytes on the stack (496 on
  the S3), on a thread with 6 to 8 KiB. A document nesting more than 6 elements, the
  root included, is refused. The check runs LVGL's own tokenizer before the tree is
  built; only an end tag that matches the innermost open element closes a level, and
  nothing closes the root, as in LVGL's parser.
- **Size.** Each side must be at most 32767 pixels and the pixels at most 4 MiB, the
  limits of the shape icons and the Lottie widget, and a row at most 65535 bytes (an
  image header's stride), so at most 16383 pixels wide.

An upload is capped by `CONFIG_EERIE_LEAP_UI_IMAGE_MAX_SIZE` alone, like every image:

```shell
tools/smp_can_client.py --serial /dev/ttyACM0 assets put images logo.svg ./logo.svg
```

## Tests

The `svg_icon` suite of `tests/functional/views` covers sizing, straight alpha,
gradients, skipped elements and every refusal. `icon_lifecycle` runs its native rotation
and anchor cases on SVG icons too, and the controllers suite checks that LVGL decodes no
SVG images.
