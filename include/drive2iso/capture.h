#ifndef D2I_CAPTURE_H
#define D2I_CAPTURE_H

#include <stdbool.h>

/* Portable capture policy: what to leave out of an image, and the on-disk shape
 * of the ISO staging tree. The actual copy (mksquashfs / wimlib) lives in the
 * backends; this module owns the rules both backends must agree on. */

/* NULL-terminated list of paths (relative to the source root) that are never
 * included in a capture: volatile kernel/vfs mounts, scratch, swap, caches, and
 * the recycle/restore areas. `os` is "linux" or "windows". Returns a static
 * array owned by the module. */
const char *const *capture_excludes(const char *os);

/* Join capture_excludes(os) into one string using `sep` between entries and
 * `prefix` before each (e.g. prefix "-e " for mksquashfs, "" for a plain list).
 * Heap-allocated; caller frees. */
char *capture_excludes_join(const char *os, const char *prefix, const char *sep);

/* Create the ISO staging tree under stage_dir for the given os (best-effort
 * mkdir of every subdir the imager expects). Honors commit: a dry-run only logs
 * the directories it would create. Returns 0 on success. */
int capture_prepare_tree(const char *stage_dir, const char *os, bool commit);

/* Write a wimlib/DISM capture-config (WIMSCRIPT) file listing the Windows
 * excludes, at `path`. Honors commit. Returns 0 on success. */
int capture_write_wim_config(const char *path, bool commit);

#endif /* D2I_CAPTURE_H */
