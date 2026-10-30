"""read turns a PDF into the text of its pages and pages through it."""

import json
import zlib

HELVETICA = (b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica "
             b"/Encoding /WinAnsiEncoding >>")


def pdf(pages, encrypt=None, pad=0):
    """A PDF with one Helvetica page per content stream in `pages`."""
    n = len(pages)
    objs = [b"<< /Type /Catalog /Pages 2 0 R >>"]
    kids = b" ".join(b"%d 0 R" % (3 + 2 * i) for i in range(n))
    objs.append(b"<< /Type /Pages /Kids [%s] /Count %d >>" % (kids, n))
    font = 3 + 2 * n
    for i, content in enumerate(pages):
        data = zlib.compress(content)
        objs.append(b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                    b"/Resources << /Font << /F1 %d 0 R >> >> "
                    b"/Contents %d 0 R >>" % (font, 4 + 2 * i))
        objs.append(b"<< /Filter /FlateDecode /Length %d >>\nstream\n"
                    % len(data) + data + b"\nendstream")
    objs.append(HELVETICA)
    trailer = b""
    if encrypt:
        objs.append(encrypt)
        trailer = (b" /Encrypt %d 0 R /ID [<00112233445566778899aabbccddeeff>"
                   b" <00112233445566778899aabbccddeeff>]" % len(objs))
    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    if pad:
        out += b"%" + b"x" * pad + b"\n"
    offsets = []
    for i, body in enumerate(objs, 1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % i + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    out += b"".join(b"%010d 00000 n \n" % off for off in offsets)
    out += (b"trailer\n<< /Size %d /Root 1 0 R%s >>\nstartxref\n%d\n%%%%EOF\n"
            % (len(objs) + 1, trailer, xref))
    return bytes(out)


def text_page(*lines):
    ops = [b"BT /F1 11 Tf 72 720 Td"]
    for i, line in enumerate(lines):
        if i:
            ops.append(b"0 -14 Td")
        ops.append(b"(" + line.encode() + b") Tj")
    ops.append(b"ET")
    return b" ".join(ops)


def read_result(ctx, name, data, **args):
    (ctx.work / name).write_bytes(data)
    call = json.dumps({"path": name, **args})
    ctx.scenario(f"tool=read:{call},final_text=pdf+read")
    s = ctx.spawn()
    s.submit(f"read {name}")
    s.wait_text("pdf read")
    s.wait_turn_done()
    return ctx.mock.tool_results()[-1]


def test_read_pdf_returns_the_text_of_each_page(ctx):
    data = pdf([text_page("First page title", "Body text here."),
                text_page("Second page")])
    result = read_result(ctx, "paper.pdf", data)
    assert result == ("--- page 1 of 2 ---\nFirst page title\nBody text here.\n"
                      "\n--- page 2 of 2 ---\nSecond page\n"), result


def test_read_pdf_pages_with_offset_and_limit(ctx):
    data = pdf([text_page("one", "two", "three"), text_page("four")])
    result = read_result(ctx, "paged.pdf", data, offset=3, limit=2)
    assert result.startswith("two\nthree\n"), result
    assert "[read 2 of 7 lines; continue with offset=5]" in result, result


def test_read_pdf_with_a_password_is_refused(ctx):
    locked = (b"<< /Filter /Standard /V 2 /R 3 /Length 128 /P -3904 "
              b"/O <" + b"11" * 32 + b"> /U <" + b"22" * 32 + b"> >>")
    result = read_result(ctx, "locked.pdf",
                         pdf([text_page("hidden")], encrypt=locked))
    assert result.startswith("ERROR:"), result
    assert "locked.pdf is a PDF, but it is protected by a password" in result, \
        result
    assert "hidden" not in result, result


def test_read_pdf_without_text_says_so(ctx):
    result = read_result(ctx, "scan.pdf", pdf([b"0 0 m 200 200 l S"]))
    assert "[no text on this page]" in result, result
    assert "[its one page has no text; it may be a scanned image" in result, \
        result


def test_read_pdf_over_the_text_file_limit(ctx):
    """A PDF may exceed the 16MB text file limit; images inside are common."""
    data = pdf([text_page("Large but fine")], pad=17 << 20)
    result = read_result(ctx, "large.pdf", data)
    assert "Large but fine" in result, result


def test_read_pdf_that_is_damaged_names_the_problem(ctx):
    result = read_result(ctx, "broken.pdf", b"%PDF-1.4\nnothing else\n")
    assert result.startswith("ERROR:"), result
    assert "broken.pdf is a PDF, but" in result, result
