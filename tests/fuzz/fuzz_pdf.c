/* libFuzzer harness for the PDF text reader.
 *
 * The input is taken as a PDF file. The reader must return without reading
 * outside it, and what it appends must stay valid UTF-8. Built by `make fuzz`
 * into bin/fuzz/fuzz_pdf; seed the corpus with small PDF files and run it with
 * `-max_total_time=60`.
 */

#define _XOPEN_SOURCE   700
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "agent.h"

void telemetry_log(i32 level, Str msg) {
    (void)level;
    (void)msg;
}

#include "core.c"
#include "pdf.c"

#include <stdint.h>
#include <stdlib.h>

static alignas(64) u8 g_fuzz_mem[1u << 26];

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static b8 valid_utf8(Str s) {
    for (size_t i = 0; i < s.n;) {
        u8 c = (u8)s.p[i];
        size_t len = c < 0x80         ? 1
                     : (c >> 5) == 6  ? 2
                     : (c >> 4) == 14 ? 3
                     : (c >> 3) == 30 ? 4
                                      : 0;
        if (!len || len > s.n - i) return false;
        for (size_t k = 1; k < len; k++)
            if (((u8)s.p[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    Arena a;
    arena_init(&a, g_fuzz_mem, sizeof g_fuzz_mem);
    Buf out;
    buf_init(&out, &a, 4096);
    PdfStats stats;
    char err[256];
    Str in = {(const char *)data, size};
    if (pdf_text(in, &a, &out, &stats, err, sizeof err) && buf_ok(&out)
        && !valid_utf8((Str){out.p, out.n}))
        abort();
    return 0;
}
