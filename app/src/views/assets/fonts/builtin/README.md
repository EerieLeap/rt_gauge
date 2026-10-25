# Built-in fonts

TrueType fonts compiled into flash and served as read-only files of the `fonts` asset kind
(`CONFIG_EERIE_LEAP_UI_BUILTIN_FONTS`, `views/assets/fonts/builtin_fonts.cpp`). Each is licensed under the SIL
Open Font License 1.1: the license text sits next to the file, and the font carries the license (name IDs 13 and 14)
and its copyright (ID 0) itself.

| File | Font | Pinned upstream release | Source SHA-256 |
| --- | --- | --- | --- |
| `Rubik-Medium.ttf` | Rubik Medium 2.300 | [googlefonts/rubik `9167c98fa8`](https://github.com/googlefonts/rubik/tree/9167c98fa8), `fonts/ttf/Rubik-Medium.ttf` | `0a72404d...0c0d2` |
| `Montserrat-Medium.ttf` | Montserrat Medium 7.222 | [JulietaUla/Montserrat `v7.222`](https://github.com/JulietaUla/Montserrat/tree/v7.222), `fonts/ttf/Montserrat-Medium.ttf` | `7ce96811...9cbc` |
| `Inconsolata-Bold.ttf` | Inconsolata Bold 3.001 | [googlefonts/Inconsolata `b07d05759f`](https://github.com/googlefonts/Inconsolata/tree/b07d05759f), `fonts/ttf/Inconsolata-Bold.ttf` | `69deb4d3...abb4` |

Each `*-OFL.txt` is the `OFL.txt` of the same revision, so its copyright line is the font's. (Montserrat 9.000, the
newest release, ships an `OFL.txt` that names "The Montserrat.Git Project Authors, 2024" while its fonts say
"The Montserrat Project Authors, 2011", so 7.222, where the two agree, is used.) The full hashes are in
`make_fonts.py`, which refuses a source that is not the pinned file.

The files are generated, not edited: `make_fonts.py` fetches the pinned sources, makes the subsets and writes
them here with the license texts, and checks the result against HarfBuzz.

```sh
pip install fonttools uharfbuzz
python3 app/src/views/assets/fonts/builtin/make_fonts.py        # sources go to ./sources (git-ignored)
```

The same sources give the same bytes. What it does to a font:

- **Latin-1 subset**, U+0020-007E and U+00A0-00FF, hinting dropped, names 0-6, 13 and 14 kept (English), no GSUB, no GDEF.
- **Kerning flattened into a `kern` table.** The unit draws with LVGL's Tiny TTF (stb_truetype), which reads a `kern`
  table only when the font has no GPOS, and reads GPOS only for plain PairPos lookups with an x advance, not through
  Extension lookups, and stops at the first subtable that covers the left glyph. The upstream fonts keep their kerning
  in Extension lookups and class-based subtables, so the unit would not kern them, or kern them wrongly. The app
  previews with Skia (HarfBuzz), which applies `kern` when there is no GPOS. So every kerned pair of the subset is
  written into a format 0 `kern` table as HarfBuzz would apply it (the first subtable of a lookup that covers the
  pair decides; the lookups of the `kern` feature add up), and GPOS is dropped. `make_fonts.py` then shapes every
  Latin-1 pair with HarfBuzz from the source and from the subset and fails unless the glyphs land in the same places.
  Inconsolata is monospace and has no kerned pair, so it has no `kern` table.

| File | Bytes | Kerned pairs |
| --- | --- | --- |
| `Rubik-Medium.ttf` | 54,188 | 5,403 |
| `Montserrat-Medium.ttf` | 68,600 | 8,871 |
| `Inconsolata-Bold.ttf` | 16,140 | 0 |

The names are the ones clients and saved configurations use (`FONT_FILE_PATH`): never rename them. The app bundles
the same files for its preview.
