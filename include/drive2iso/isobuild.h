#ifndef D2I_ISOBUILD_H
#define D2I_ISOBUILD_H

#include <stdbool.h>

/* Turn a prepared staging tree into a bootable ISO by constructing the right
 * imager command line. Pure string construction — the backend runs it (dry-run
 * or commit). Paths are double-quoted, which is safe for both /bin/sh and
 * cmd.exe; keep stage/out paths free of embedded double quotes. */

typedef struct {
    bool have_grub_mkrescue; /* Linux: preferred one-shot BIOS+UEFI isohybrid builder */
    bool have_xorriso;       /* fallback imager (also builds the Windows El Torito ISO) */
    bool have_oscdimg;       /* Windows: ADK imager (etfsboot + efisys) */
} iso_tools_t;

/* style: "linux"  -> grub-mkrescue (preferred) or xorriso isohybrid, BIOS+UEFI.
 *        "windows" -> xorriso/oscdimg El Torito with etfsboot.com + efisys.bin.
 * Returns a heap command string (caller frees), or NULL if no suitable imager
 * is available in `tools` (the caller then reports what to install). */
char *isobuild_command(const char *stage_dir, const char *out_iso,
                       const char *label, const char *style,
                       bool isohybrid, const iso_tools_t *tools);

#endif /* D2I_ISOBUILD_H */
