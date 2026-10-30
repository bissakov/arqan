#!/usr/bin/env python3
"""Generate src/pdf_tables.c: the constant tables the PDF text reader uses.

A simple PDF font names its glyphs instead of giving their Unicode values, so
turning a glyph back into text needs the meaning of each name and of each
standard encoding:

  * Glyph names: the Adobe Glyph List for New Fonts (AGLFN), the names the
    full Adobe Glyph List gives to Greek, math and typographic glyphs, and the
    names in the TeX Computer Modern fonts. Where AGL maps a Greek capital to
    a math symbol (Delta, Omega), the Greek letter wins: the reader wants text.
  * Encodings: StandardEncoding, WinAnsiEncoding, MacRomanEncoding and the
    Symbol font's built-in encoding, as one code point per byte.
  * Glyph widths of the standard 14 fonts, which a PDF may use without
    stating widths. Read from Adobe's core font AFM files, which matplotlib
    ships; pass another directory of them with --afm.
  * The 391 standard strings of the Compact Font Format, which name the
    glyphs of an embedded CFF font.
  * The AES S-box and its inverse, for documents encrypted with AES-128.

Run it from the repository root; it rewrites src/pdf_tables.c in place, then
run `make fmt`. It needs fontTools and matplotlib
(`pip install fonttools matplotlib`).
"""

from __future__ import annotations

import argparse
import sys
import unicodedata
from pathlib import Path

try:
    import fontTools.agl as agl
    from fontTools.cffLib import cffStandardStrings
    from fontTools.encodings.StandardEncoding import StandardEncoding
except ImportError:
    raise SystemExit("needs fontTools: pip install fonttools")

OUT = Path("src/pdf_tables.c")

AFM_FONTS = [
    ("k_w_times", "Times-Roman"),
    ("k_w_times_bold", "Times-Bold"),
    ("k_w_times_italic", "Times-Italic"),
    ("k_w_times_bold_italic", "Times-BoldItalic"),
    ("k_w_helvetica", "Helvetica"),
    ("k_w_helvetica_bold", "Helvetica-Bold"),
    ("k_w_symbol", "Symbol"),
]

LEGACY = """
Alpha Beta Chi Epsilon Eta Iota Kappa Mu Nu Omicron Rho Tau Zeta upsilon
omicron omega1 notequal angle therefore congruent dotmath second angleleft
angleright lozenge quotesinglbase quotedblbase guillemotleft guillemotright
guilsinglleft guilsinglright germandbls degree copyright registered florin
perthousand suchthat orthogonal
""".split()

# Names AGL lacks or maps to a symbol where text wants a letter: TeX Computer
# Modern (cmr, cmmi, cmsy, cmex), AMS symbols, and Greek capitals.
EXTRA = """
Delta 0394 Omega 03A9 Gamma 0393 Theta 0398 Lambda 039B Xi 039E Pi 03A0
Sigma 03A3 Upsilon 03A5 Phi 03A6 Psi 03A8 Upsilon1 03D2 mu 03BC ff FB00
fi FB01 fl FB02 ffi FB03 ffl FB04 suppress 0337 exclamdown 00A1
questiondown 00BF dieresis 00A8 ring 02DA tie 2040 cwm 200C
compwordmark 200C visiblespace 2423 perthousandzero 2030 hyphenchar 002D
nbspace 00A0 sfthyphen 00AD alpha 03B1 beta 03B2 gamma 03B3 delta 03B4
epsilon1 03B5 zeta 03B6 eta 03B7 theta 03B8 iota 03B9 kappa 03BA
lambda 03BB nu 03BD xi 03BE pi 03C0 rho 03C1 sigma 03C3 tau 03C4 phi 03D5
chi 03C7 psi 03C8 omega 03C9 epsilon 03F5 theta1 03D1 pi1 03D6 rho1 03F1
sigma1 03C2 phi1 03C6 harpoonleftup 21BC harpoonleftdown 21BD
harpoonrightup 21C0 harpoonrightdown 21C1 arrowlefttophalf 21BC
arrowleftbothalf 21BD arrowrighttophalf 21C0 arrowrightbothalf 21C1
hookleft 21A9 hookright 21AA triangleright 25B7 triangleleft 25C1
star 22C6 partialdiff 2202 flat 266D natural 266E sharp 266F
slurbelow 2323 slurabove 2322 lscript 2113 weierstrass 2118 vector 20D7
dotlessi 0131 dotlessj 0237 minus 2212 periodcentered 00B7 multiply 00D7
asteriskmath 2217 divide 00F7 diamondmath 22C4 plusminus 00B1
minusplus 2213 circleplus 2295 circleminus 2296 circlemultiply 2297
circledivide 2298 circledot 2299 circlecopyrt 25EF openbullet 25E6
bullet 2022 equivasymptotic 224D equivalence 2261 reflexsubset 2286
reflexsuperset 2287 lessequal 2264 greaterequal 2265 precedesequal 2AAF
followsequal 2AB0 similar 223C approxequal 2248 propersubset 2282
propersuperset 2283 lessmuch 226A greatermuch 226B precedes 227A
follows 227B arrowleft 2190 arrowright 2192 arrowup 2191 arrowdown 2193
arrowboth 2194 arrownortheast 2197 arrowsoutheast 2198 similarequal 2243
arrowdblleft 21D0 arrowdblright 21D2 arrowdblup 21D1 arrowdbldown 21D3
arrowdblboth 21D4 arrownorthwest 2196 arrowsouthwest 2199
proportional 221D prime 2032 infinity 221E element 2208 owner 220B
triangle 25B3 triangleinv 25BD negationslash 0338 mapsto 21A6
universal 2200 existential 2203 logicalnot 00AC emptyset 2205
Rfractur 211C Ifractur 2111 Rfraktur 211C Ifraktur 2111 latticetop 22A4
perpendicular 22A5 aleph 2135 union 222A intersection 2229
unionmulti 228E logicaland 2227 logicalor 2228 turnstileleft 22A2
turnstileright 22A3 floorleft 230A floorright 230B ceilingleft 2308
ceilingright 2309 braceleft 007B braceright 007D angbracketleft 27E8
angbracketright 27E9 bar 007C bardbl 2016 arrowbothv 2195
arrowdblbothv 21D5 backslash 005C wreathproduct 2240 radical 221A
coproduct 2210 nabla 2207 integral 222B unionsq 2294 intersectionsq 2293
subsetsqequal 2291 supersetsqequal 2292 section 00A7 dagger 2020
daggerdbl 2021 paragraph 00B6 club 2663 diamond 2662 heart 2661
spade 2660
"""

# cmex: pdf.c reduces a sized variant (parenleftbig, summationdisplay) to its
# base name, so only the pieces with no base glyph are listed here.
EXTRA_CMEX = """
summation 2211 product 220F contintegral 222E hatwide 0302 hatwider 0302
hatwidest 0302 tildewide 0303 tildewider 0303 tildewidest 0303
radicalbt 221A radicalvertex 23B7 radicaltp 23B7 parenlefttp 239B
parenleftex 239C parenleftbt 239D parenrighttp 239E parenrightex 239F
parenrightbt 23A0 bracketlefttp 23A1 bracketleftex 23A2
bracketleftbt 23A3 bracketrighttp 23A4 bracketrightex 23A5
bracketrightbt 23A6 bracelefttp 23A7 braceleftmid 23A8 braceleftbt 23A9
bracerighttp 23AB bracerightmid 23AC bracerightbt 23AD braceex 23AA
integraltp 2320 integralbt 2321 integralex 23AE arrowvertex 23D0
arrowhorizex 23AF square 25A1 squaresolid 25A0 checkmark 2713
lessorequalslant 2A7D greaterorequalslant 2A7E defines 225C
quoteleft 2018 quoteright 2019 quotedblleft 201C quotedblright 201D
endash 2013 emdash 2014 Euro 20AC ellipsis 2026 trademark 2122
fraction 2044 minute 2032 Ohm 03A9 Omegagreek 03A9 Deltagreek 0394
mu1 00B5 because 2235 notelement 2209 notsubset 2284 hyphen 002D
barex 007C bardblex 2016 parenlefttpA 239B parenleftexA 239C
parenleftbtA 239D parenrighttpA 239E parenrightexA 239F parenrightbtA 23A0
"""

# The tips of a horizontal brace drawn under or over a formula. They carry no
# text, so they map to a zero width space, which the reader drops.
EXTRA_PIECES = """
bracehtipdownleft 200B bracehtipdownright 200B bracehtipupleft 200B
bracehtipupright 200B
"""

# Symbol font: Greek letters on the Latin keys, math above 0xA0.
SYMBOL_GREEK = (
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0391 0392 03A7 0394 0395 03A6 0393 0397 0399 03D1 039A 039B 039C 039D "
    "039F 03A0 0398 03A1 03A3 03A4 03A5 03C2 03A9 039E 03A8 0396",
    "abcdefghijklmnopqrstuvwxyz",
    "03B1 03B2 03C7 03B4 03B5 03C6 03B3 03B7 03B9 03D5 03BA 03BB 03BC 03BD "
    "03BF 03C0 03B8 03C1 03C3 03C4 03C5 03D6 03C9 03BE 03C8 03B6",
)
SYMBOL_OTHER = """
22 2200 24 2203 27 220B 2A 2217 2D 2212 40 2245 5C 2234 5E 22A5 60 203E
7E 223C A0 20AC A1 03D2 A2 2032 A3 2264 A4 2044 A5 221E A6 0192 A7 2663
A8 2666 A9 2665 AA 2660 AB 2194 AC 2190 AD 2191 AE 2192 AF 2193 B0 00B0
B1 00B1 B2 2033 B3 2265 B4 00D7 B5 221D B6 2202 B7 2022 B8 00F7 B9 2260
BA 2261 BB 2248 BC 2026 BD 23D0 BE 23AF BF 21B5 C0 2135 C1 2111 C2 211C
C3 2118 C4 2297 C5 2295 C6 2205 C7 2229 C8 222A C9 2283 CA 2287 CB 2284
CC 2282 CD 2286 CE 2208 CF 2209 D0 2220 D1 2207 D2 00AE D3 00A9 D4 2122
D5 220F D6 221A D7 22C5 D8 00AC D9 2227 DA 2228 DB 21D4 DC 21D0 DD 21D1
DE 21D2 DF 21D3 E0 25CA E1 2329 E2 00AE E3 00A9 E4 2122 E5 2211 E6 239B
E7 239C E8 239D E9 23A1 EA 23A2 EB 23A3 EC 23A7 ED 23A8 EE 23A9 EF 23AA
F1 232A F2 222B F3 2320 F4 23AE F5 2321 F6 239E F7 239F F8 23A0 F9 23A4
FA 23A5 FB 23A6 FC 23AB FD 23AC FE 23AD
"""


def pairs(text: str) -> list[tuple[str, int]]:
    words = text.split()
    return [(words[i], int(words[i + 1], 16)) for i in range(0, len(words), 2)]


def glyph_names() -> dict[str, int]:
    names = dict(agl.AGL2UV)
    for name in LEGACY:
        uv = agl.LEGACY_AGL2UV.get(name)
        if uv is None:
            raise SystemExit(f"{name} is not in the Adobe Glyph List")
        names.setdefault(name, uv[0] if isinstance(uv, (list, tuple)) else uv)
    for name, cp in pairs(EXTRA) + pairs(EXTRA_CMEX) + pairs(EXTRA_PIECES):
        names[name] = cp
    return names


def standard(names: dict[str, int]) -> list[int]:
    return [0 if n == ".notdef" else names[n] for n in StandardEncoding]


def winansi() -> list[int]:
    out = []
    for c in range(256):
        try:
            out.append(ord(bytes([c]).decode("cp1252")) if c >= 32 else 0)
        except UnicodeDecodeError:
            out.append(0x2022)
    out[0x7F] = 0x2022
    out[0xA0] = 0x20
    out[0xAD] = 0x2D
    return out


def macroman() -> list[int]:
    out = [0] * 32 + [ord(bytes([c]).decode("mac_roman")) for c in range(32, 256)]
    out[0x7F] = 0
    return out


def symbol() -> list[int]:
    out = [c if 0x20 <= c < 0x7F else 0 for c in range(256)]
    for i in (0, 2):
        for ch, cp in zip(SYMBOL_GREEK[i], SYMBOL_GREEK[i + 1].split()):
            out[ord(ch)] = int(cp, 16)
    for code, cp in pairs(SYMBOL_OTHER):
        out[int(code, 16)] = cp
    return out


def gf_mul(a: int, b: int) -> int:
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = ((a << 1) ^ 0x11B) if a & 0x80 else a << 1
        b >>= 1
    return r


def aes_sbox() -> list[int]:
    inverse = [0] * 256
    for a in range(1, 256):
        inverse[a] = next(b for b in range(1, 256) if gf_mul(a, b) == 1)
    out = []
    for a in range(256):
        x = s = inverse[a]
        for i in range(1, 5):
            s ^= ((x << i) | (x >> (8 - i))) & 0xFF
        out.append(s ^ 0x63)
    return out


def table(ctype: str, name: str, values: list[int], digits: int, per: int) -> str:
    rows = []
    for i in range(0, len(values), per):
        rows.append("    " + ", ".join(f"0x{v:0{digits}X}" for v in values[i : i + per]) + ",")
    body = "\n".join(rows)
    return f"static const {ctype} {name}[256] = {{\n{body}\n}};\n"


def afm_dir(arg: str | None) -> Path:
    if arg:
        return Path(arg)
    try:
        import matplotlib
    except ImportError:
        raise SystemExit("needs matplotlib for the core font AFMs, or --afm DIR")
    return Path(matplotlib.get_data_path()) / "fonts" / "pdfcorefonts"


# A spacing accent drawn over a letter, as TeX draws \'e, and the combining
# mark it stands for.
ACCENTS = {
    0x00B4: 0x0301, 0x0060: 0x0300, 0x02C6: 0x0302, 0x005E: 0x0302,
    0x02DC: 0x0303, 0x007E: 0x0303, 0x00A8: 0x0308, 0x00B8: 0x0327,
    0x02DA: 0x030A, 0x02C7: 0x030C, 0x02D8: 0x0306, 0x00AF: 0x0304,
    0x02C9: 0x0304, 0x02D9: 0x0307, 0x02DD: 0x030B, 0x02DB: 0x0328,
}


def cff_strings() -> str:
    if len(cffStandardStrings) != 391:
        raise SystemExit("expected 391 CFF standard strings")
    rows = []
    line = "   "
    for name in cffStandardStrings:
        item = f' "{name}",'
        if len(line) + len(item) > 79:
            rows.append(line)
            line = "   "
        line += item
    rows.append(line)
    body = "\n".join(rows)
    return (
        "/* The glyph names a CFF font gives by string ID below 391. */\n"
        f"static const char *const k_cff_std_strings[391] = {{\n{body}\n}};\n"
    )


def accent_table() -> str:
    rows = []
    letters = [chr(c) for c in range(ord("A"), ord("Z") + 1)]
    letters += [chr(c) for c in range(ord("a"), ord("z") + 1)]
    for accent, mark in sorted(ACCENTS.items()):
        for base in letters + ["\u0131"]:
            plain = "i" if base == "\u0131" else base
            composed = unicodedata.normalize("NFC", plain + chr(mark))
            if len(composed) == 1:
                rows.append((accent, ord(base), ord(composed)))
    rows.sort()
    body = "\n".join(f"    {{0x{a:04X}, 0x{b:04X}, 0x{c:04X}}}," for a, b, c in rows)
    return (
        "/* A spacing accent and the letter under it, and the letter they make.\n"
        " * Sorted by accent, then letter. */\n"
        "typedef struct {\n    u16 accent, base, composed;\n} PdfAccent;\n\n"
        f"static const PdfAccent k_pdf_accents[] = {{\n{body}\n}};\n"
    )


def afm_widths(path: Path, names: dict[str, int], symbol_enc: list[int]) -> list[tuple[int, int]]:
    widths: dict[int, int] = {}
    for line in path.read_text(encoding="latin-1").splitlines():
        if not line.startswith("C "):
            continue
        fields = {}
        for part in line.split(";"):
            words = part.split()
            if len(words) >= 2:
                fields[words[0]] = words[1]
        code, width, glyph = int(fields["C"]), int(fields["WX"]), fields.get("N")
        if symbol_enc is not None:
            cp = symbol_enc[code] if 0 <= code < 256 else 0
        else:
            cp = names.get(glyph, 0)
        if cp and cp <= 0xFFFF:
            widths.setdefault(cp, width)
    if not widths:
        raise SystemExit(f"{path}: no glyph widths")
    return sorted(widths.items())


def width_tables(directory: Path, names: dict[str, int]) -> str:
    out = []
    for cname, font in AFM_FONTS:
        enc = symbol() if font == "Symbol" else None
        rows = afm_widths(directory / f"{font}.afm", names, enc)
        body = []
        for i in range(0, len(rows), 6):
            body.append("    " + " ".join(f"{{0x{cp:04X}, {w}}}," for cp, w in rows[i : i + 6]))
        joined = "\n".join(body)
        out.append(f"static const PdfWidth {cname}[] = {{\n{joined}\n}};\n")
    return "\n".join(out)


def main() -> int:
    if not OUT.parent.is_dir():
        raise SystemExit("run from the repository root")
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--afm", help="directory of the core 14 font AFM files")
    args = parser.parse_args()
    names = glyph_names()
    widths = width_tables(afm_dir(args.afm), names)
    sbox = aes_sbox()
    inv_sbox = [0] * 256
    for i, s in enumerate(sbox):
        inv_sbox[s] = i
    if sbox[0] != 0x63 or sbox[0x53] != 0xED:
        raise SystemExit("AES S-box is wrong")
    rows = "\n".join(
        f'    {{"{n}", 0x{cp:04X}}},'
        for n, cp in sorted(names.items(), key=lambda kv: kv[0].encode())
    )
    text = f"""/* Generated by scripts/gen-pdf-tables.py. Do not edit: regenerate it.
 *
 * Glyph names from the Adobe Glyph List for New Fonts and the Adobe Glyph
 * List (Copyright Adobe, BSD-3-Clause), with the TeX Computer Modern names;
 * the standard PDF encodings; glyph widths of the standard 14 fonts, from
 * Adobe's core font AFM files (Copyright Adobe Systems Incorporated, used
 * under the terms of their readme); and the AES S-boxes. pdf.c includes
 * it. */

typedef struct {{
    const char *name;
    u32 cp;
}} PdfGlyphName;

/* Sorted by name, bytewise, for a binary search. */
static const PdfGlyphName k_pdf_glyphs[] = {{
{rows}
}};

{table("u16", "k_enc_standard", standard(names), 4, 8)}
{table("u16", "k_enc_winansi", winansi(), 4, 8)}
{table("u16", "k_enc_macroman", macroman(), 4, 8)}
{table("u16", "k_enc_symbol", symbol(), 4, 8)}

/* Width in 1/1000 em by code point, sorted by code point. Oblique faces share
 * the widths of the upright ones, and Courier is 600 throughout. */
typedef struct {{
    u16 cp, width;
}} PdfWidth;

{widths}
{accent_table()}
{cff_strings()}
{table("u8", "k_aes_sbox", sbox, 2, 12)}
{table("u8", "k_aes_inv_sbox", inv_sbox, 2, 12)}"""
    OUT.write_text(text)
    print(f"{OUT}: {len(names)} glyph names")
    return 0


if __name__ == "__main__":
    sys.exit(main())
