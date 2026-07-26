#ifndef D2I_FLASH_H
#define D2I_FLASH_H

#include <stddef.h>
#include <stdbool.h>

/* A raw block device to stream an ISO into. The backend supplies these over its
 * platform handle (a plain fd on Linux; a locked PhysicalDrive on Windows). */
typedef struct {
    void  *ctx;
    size_t align;   /* write/read length must be a multiple of this (>= 1) */
    long (*write) (void *ctx, const void *buf, size_t len); /* full-write, -1 on error */
    long (*read)  (void *ctx, void *buf, size_t len);       /* for verify; may be NULL */
    int  (*rewind)(void *ctx);                              /* seek to 0; may be NULL */
    void (*close) (void *ctx);
} flash_dev_t;

/* Stream `iso` into dev with an Etcher-style progress bar; if `verify` and the
 * device supports read+rewind, read it back and compare. Does NOT close dev.
 * Returns 0 on success. */
int flash_iso_to_dev(const char *iso, flash_dev_t *dev, bool verify);

#endif /* D2I_FLASH_H */
