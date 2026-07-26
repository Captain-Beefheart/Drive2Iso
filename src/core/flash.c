#define _FILE_OFFSET_BITS 64

#include "drive2iso/flash.h"
#include "drive2iso/ui.h"
#include "drive2iso/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#  define FSEEK64 _fseeki64
#  define FTELL64 _ftelli64
#else
#  define FSEEK64 fseeko
#  define FTELL64 ftello
#endif

#define BLOCK (4u * 1024u * 1024u)

static uint64_t file_size(FILE *f) {
    if (FSEEK64(f, 0, SEEK_END) != 0) return 0;
    long long e = FTELL64(f);
    FSEEK64(f, 0, SEEK_SET);
    return e < 0 ? 0 : (uint64_t)e;
}

static size_t pad_up(size_t n, size_t align) {
    if (align <= 1) return n;
    return ((n + align - 1) / align) * align;
}

int flash_iso_to_dev(const char *iso, flash_dev_t *dev, bool verify) {
    FILE *f = fopen(iso, "rb");
    if (!f) { log_err("cannot open %s", iso); return -1; }
    uint64_t total = file_size(f);
    if (total == 0) { log_err("empty or unreadable ISO: %s", iso); fclose(f); return -1; }

    unsigned char *a = malloc(BLOCK);
    unsigned char *b = malloc(BLOCK);
    if (!a || !b) { free(a); free(b); fclose(f); return -1; }

    int rc = 0;
    uint64_t done = 0;

    for (;;) {
        size_t n = fread(a, 1, BLOCK, f);
        if (n == 0) break;
        size_t w = pad_up(n, dev->align);
        if (w > n) memset(a + n, 0, w - n);
        long wr = dev->write(dev->ctx, a, w);
        if (wr < 0 || (size_t)wr < w) {
            log_err("write failed at %llu MiB", (unsigned long long)(done / (1024 * 1024)));
            rc = -1; break;
        }
        done += n;
        ui_progress("writing", done, total);
    }
    fputc('\n', stderr);

    if (rc == 0 && verify) {
        if (!dev->read || !dev->rewind) {
            log_warn("device cannot be read back; skipping verify");
        } else if (dev->rewind(dev->ctx) != 0 || FSEEK64(f, 0, SEEK_SET) != 0) {
            log_err("verify: rewind failed"); rc = -1;
        } else {
            uint64_t vd = 0;
            for (;;) {
                size_t n = fread(a, 1, BLOCK, f);
                if (n == 0) break;
                size_t r = pad_up(n, dev->align);
                long rd = dev->read(dev->ctx, b, r);
                if (rd < 0 || (size_t)rd < r) {
                    log_err("verify: read failed at %llu MiB", (unsigned long long)(vd / (1024 * 1024)));
                    rc = -1; break;
                }
                if (memcmp(a, b, n) != 0) {
                    log_err("verify MISMATCH at %llu MiB", (unsigned long long)(vd / (1024 * 1024)));
                    rc = -1; break;
                }
                vd += n;
                ui_progress("verify ", vd, total);
            }
            fputc('\n', stderr);
        }
    }

    free(a); free(b); fclose(f);
    if (rc == 0) log_info("flashed %llu MiB%s", (unsigned long long)(total / (1024 * 1024)),
                          verify ? " (verified)" : "");
    return rc;
}
