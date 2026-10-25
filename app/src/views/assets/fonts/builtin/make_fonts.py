#!/usr/bin/env python3
"""Builds the gauge's built-in fonts: Latin-1 subsets of three pinned upstream fonts, with the kerning the unit can use.

    python3 make_fonts.py [--src DIR] [--out DIR] [--no-verify]

The sources are downloaded from the pinned upstream revisions below into --src (default: a "sources" directory beside
this script, not checked in) and checked against their SHA-256; --out (default: this script's directory) gets the
three subsets and the OFL texts. Needs fontTools (and uharfbuzz for the verification): pip install fonttools uharfbuzz.

Why the kerning is rewritten. The unit draws text with LVGL's Tiny TTF, which uses stb_truetype: it reads a font's
`kern` table only when the font has no GPOS table, and from GPOS it reads only PairPos lookups (type 2, not through
Extension lookups of type 9) with an x-advance in the first value record, stopping at the first subtable that
covers the left glyph. The upstream fonts keep their kerning in GPOS, mostly in Extension lookups and class-based
subtables, so the unit would draw them unkerned or wrongly. The app previews with Skia, which uses HarfBuzz. So
each subset gets its kerning flattened into a format 0 `kern` table, pair by pair, as HarfBuzz would apply it to
horizontal left-to-right Latin text (a font with no kerned pair, a monospace one, gets no `kern` table), and loses its
GPOS table: stb_truetype reads the `kern` table, HarfBuzz
applies `kern` when there is no GPOS, and both place the glyphs alike.

How a pair is kerned (HarfBuzz, for the `kern` feature of the `latn` script, or `DFLT` without it): each lookup
the feature names is tried in turn on the pair; within a lookup the first subtable whose coverage holds the left
glyph decides (a format 1 subtable that does not list the right glyph passes the pair on to the next subtable, a
format 2 subtable always decides, even with a zero value); the values of the lookups add up. Only the x advance of
the first glyph is kept. A font that needs more (a value on the second glyph, an x placement) makes the script stop.
"""

import argparse
import hashlib
import io
import sys
import urllib.request
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._k_e_r_n import KernTable_format_0

# The Latin-1 repertoire: ASCII printable and the Latin-1 Supplement from the no-break space.
UNICODES = "U+0020-007E,U+00A0-00FF"
# Copyright, family, subfamily, unique ID, full name, version, postscript name, license text and license URL: the
# SIL Open Font License travels inside every font file.
NAME_IDS = [0, 1, 2, 3, 4, 5, 6, 13, 14]

# The sources, pinned: (output name, font URL, font SHA-256, license URL, license SHA-256, license file name).
FONTS = [
    (
        "Rubik-Medium",
        "https://raw.githubusercontent.com/googlefonts/rubik/9167c98fa8/fonts/ttf/Rubik-Medium.ttf",
        "0a72404de99c51a873eeb4c1f6fb6188a29716911f44dada1b8fa56d79f0c0d2",
        "https://raw.githubusercontent.com/googlefonts/rubik/9167c98fa8/OFL.txt",
        "472cbe7c25441df63e9c7864b43eb3c0f4b3df950c66a76224e6cfe1eae843fb",
        "Rubik-OFL.txt",
    ),
    (
        "Montserrat-Medium",
        "https://raw.githubusercontent.com/JulietaUla/Montserrat/v7.222/fonts/ttf/Montserrat-Medium.ttf",
        "7ce96811837174f00c087b73332aed3f04a19069248ab46213d9ea05ff879cbc",
        "https://raw.githubusercontent.com/JulietaUla/Montserrat/v7.222/OFL.txt",
        "41f82bb4d24b304f30f7136bc47abdd083782e4265c984160f5649d1e78ea49c",
        "Montserrat-OFL.txt",
    ),
    (
        "Inconsolata-Bold",
        "https://raw.githubusercontent.com/googlefonts/Inconsolata/b07d05759f/fonts/ttf/Inconsolata-Bold.ttf",
        "69deb4d3626bcdce00331458e62de49e0ae58236626841014939dd8c5119abb4",
        "https://raw.githubusercontent.com/googlefonts/Inconsolata/b07d05759f/OFL.txt",
        "5d362a6f8690517fd9a5573128a081d8bbbb2f92714cf00556e08fbbe9600426",
        "Inconsolata-OFL.txt",
    ),
]


def download(url: str, sha256: str, destination: Path) -> bytes:
    """The pinned file: fetched when it is not in the source directory, and always checked against its SHA-256."""
    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        with urllib.request.urlopen(url) as response:  # noqa: S310 - pinned https URLs above
            destination.write_bytes(response.read())
    data = destination.read_bytes()
    if hashlib.sha256(data).hexdigest() != sha256:
        raise SystemExit(f"{destination} is not the file {url} pinned (SHA-256 {sha256})")
    return data


# ---- the kerning the font's GPOS table gives a pair ------------------------------------------------------------------


def kern_lookup_indices(gpos) -> list[int]:
    """The lookups of the `kern` feature that HarfBuzz runs for default-language Latin text."""
    scripts = {record.ScriptTag: record.Script for record in gpos.ScriptList.ScriptRecord}
    script = scripts.get("latn") or scripts.get("DFLT")
    if script is None or script.DefaultLangSys is None:
        raise SystemExit("the font has no default language system for latn or DFLT")

    langsys = script.DefaultLangSys
    feature_indices = list(langsys.FeatureIndex)
    if langsys.ReqFeatureIndex != 0xFFFF:
        feature_indices.append(langsys.ReqFeatureIndex)

    lookups: set[int] = set()
    for index in feature_indices:
        record = gpos.FeatureList.FeatureRecord[index]
        if record.FeatureTag == "kern":
            lookups.update(record.Feature.LookupListIndex)
    return sorted(lookups)


def pair_subtables(gpos, lookup_index: int) -> list:
    """The PairPos subtables of a lookup, through Extension subtables, in order."""
    lookup = gpos.LookupList.Lookup[lookup_index]
    subtables = []
    for subtable in lookup.SubTable:
        if lookup.LookupType == 9:
            if subtable.ExtensionLookupType != 2:
                raise SystemExit(f"lookup {lookup_index}: extension of type {subtable.ExtensionLookupType}")
            subtable = subtable.ExtSubTable
        elif lookup.LookupType != 2:
            raise SystemExit(f"lookup {lookup_index}: type {lookup.LookupType} in the kern feature")
        subtables.append(subtable)
    return subtables


def x_advance(record) -> int:
    for field in ("XPlacement", "YPlacement", "YAdvance"):
        if getattr(record, field, 0):
            raise SystemExit(f"a kerning value uses {field}, which the kern table cannot hold")
    return getattr(record, "XAdvance", 0) or 0


def check_second_value(record) -> None:
    if record is not None and any(getattr(record, f, 0) for f in ("XPlacement", "YPlacement", "XAdvance", "YAdvance")):
        raise SystemExit("a kerning pair adjusts its second glyph, which the kern table cannot hold")


def flatten_kerning(font: TTFont) -> dict[tuple[str, str], int]:
    """Every kerned pair of the font's glyphs, as HarfBuzz would kern it, with its x advance."""
    gpos = font["GPOS"].table
    glyphs = font.getGlyphOrder()
    lookups = kern_lookup_indices(gpos)

    # Per subtable: the left glyphs it covers, and what it says for a right glyph.
    pairs: dict[tuple[str, str], int] = {}
    for lookup_index in lookups:
        decided: set[tuple[str, str]] = set()  # pairs an earlier subtable of this lookup decided
        for subtable in pair_subtables(gpos, lookup_index):
            covered = subtable.Coverage.glyphs
            if subtable.Format == 1:
                for left, pair_set in zip(covered, subtable.PairSet):
                    for record in pair_set.PairValueRecord:
                        key = (left, record.SecondGlyph)
                        if key in decided:
                            continue
                        decided.add(key)
                        check_second_value(record.Value2)
                        value = x_advance(record.Value1) if record.Value1 is not None else 0
                        if value:
                            pairs[key] = pairs.get(key, 0) + value
            elif subtable.Format == 2:
                class1 = subtable.ClassDef1.classDefs
                class2 = subtable.ClassDef2.classDefs
                for left in covered:
                    left_record = subtable.Class1Record[class1.get(left, 0)]
                    for right in glyphs:
                        key = (left, right)
                        if key in decided:
                            continue
                        decided.add(key)
                        record = left_record.Class2Record[class2.get(right, 0)]
                        check_second_value(record.Value2)
                        value = x_advance(record.Value1) if record.Value1 is not None else 0
                        if value:
                            pairs[key] = pairs.get(key, 0) + value
            else:
                raise SystemExit(f"PairPos format {subtable.Format}")
    return {key: value for key, value in pairs.items() if value}


# ---- build -----------------------------------------------------------------------------------------------------------


def subset_font(source: bytes) -> TTFont:
    options = subset.Options()
    options.layout_features = ["kern"]  # the kerning is flattened below; no other feature is kept
    options.hinting = False
    options.name_IDs = NAME_IDS
    options.name_languages = [0x0409]  # English (United States)
    options.notdef_outline = True
    options.glyph_names = False
    options.legacy_kern = False
    options.drop_tables += ["DSIG", "GDEF", "GSUB", "FFTM", "meta", "STAT", "gasp", "prep", "fpgm", "cvt "]
    # recalcTimestamp=False keeps head.modified as the source has it, so the same source gives the same bytes.
    font = TTFont(io.BytesIO(source), recalcTimestamp=False)
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=subset.parse_unicodes(UNICODES))
    subsetter.subset(font)
    return font


def add_kern_table(font: TTFont, pairs: dict[tuple[str, str], int]) -> None:
    table = newTable("kern")
    table.version = 0
    subtable = KernTable_format_0()
    subtable.version = 0
    subtable.coverage = 1  # horizontal, kerning values (not minimum), no cross-stream
    subtable.format = 0
    subtable.kernTable = dict(pairs)
    table.kernTables = [subtable]
    font["kern"] = table


def build(source: bytes) -> tuple[bytes, int]:
    font = subset_font(source)
    pairs = flatten_kerning(font)
    if len(pairs) > 0xFFFF:
        raise SystemExit(f"{len(pairs)} kerned pairs; a format 0 kern table holds 65535")
    if pairs:
        add_kern_table(font, pairs)
    del font["GPOS"]
    output = io.BytesIO()
    font.save(output)
    return output.getvalue(), len(pairs)


# ---- verification ----------------------------------------------------------------------------------------------------


def verify(original: bytes, built: bytes) -> int:
    """Shapes every Latin-1 pair with HarfBuzz from both fonts and compares the advances. Returns the pairs kerned."""
    import uharfbuzz as hb

    def shaper(data: bytes):
        font = hb.Font(hb.Face(data))
        return font

    def placed(font, text: str) -> list[int]:
        """Where each glyph's origin lies, then the total advance: HarfBuzz splits a kern table's value between the
        advances of both glyphs and moves the second by its half, so only the positions compare between a GPOS pair
        adjustment and a kern table."""
        buffer = hb.Buffer()
        buffer.add_str(text)
        buffer.guess_segment_properties()
        hb.shape(font, buffer, {"kern": True, "liga": False, "calt": False})
        pen = 0
        positions = []
        for position in buffer.glyph_positions:
            positions.append(pen + position.x_offset)
            pen += position.x_advance
        return positions + [pen]

    reference = shaper(original)
    candidate = shaper(built)
    # The soft hyphen (U+00AD) is skipped: HarfBuzz hides it and kerns around it, and Tiny TTF draws it as a glyph.
    characters = [chr(c) for c in list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) if c != 0xAD]
    kerned = 0
    for left in characters:
        for right in characters:
            expected = placed(reference, left + right)
            actual = placed(candidate, left + right)
            if expected != actual:
                raise SystemExit(
                    f"{left!r} {right!r}: HarfBuzz places {expected} from the source, {actual} from the subset"
                )
            if expected[-1] != placed(reference, left)[-1] + placed(reference, right)[-1]:
                kerned += 1
    return kerned


def main() -> None:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--src", type=Path, default=here / "sources", help="where the pinned sources are kept")
    parser.add_argument("--out", type=Path, default=here, help="where the subsets and licenses go")
    parser.add_argument("--no-verify", action="store_true", help="skip the HarfBuzz comparison")
    arguments = parser.parse_args()

    for name, font_url, font_sha256, license_url, license_sha256, license_name in FONTS:
        source = download(font_url, font_sha256, arguments.src / f"{name}.ttf")
        license_text = download(license_url, license_sha256, arguments.src / license_name)
        built, pair_count = build(source)

        (arguments.out / f"{name}.ttf").write_bytes(built)
        (arguments.out / license_name).write_bytes(license_text)
        line = f"{name}: {len(built)} bytes, {pair_count} kerned pairs, sha256 {hashlib.sha256(built).hexdigest()}"
        if not arguments.no_verify:
            line += f", {verify(source, built)} Latin-1 pairs checked against HarfBuzz on the source"
        print(line)


if __name__ == "__main__":
    sys.exit(main())
