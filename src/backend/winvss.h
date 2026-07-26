#ifndef D2I_WINVSS_H
#define D2I_WINVSS_H

#include <stdbool.h>

/* A consistent point-in-time view of a live NTFS volume via the Volume Shadow
 * Copy Service. Driven through the Win32_ShadowCopy WMI class (via PowerShell)
 * rather than diskshadow.exe, which ships only on Windows Server — so this works
 * on every client edition and builds cleanly under MinGW (no COM). Needs an
 * elevated prompt.
 *
 * winvss_create() takes a ClientAccessible shadow of `volume` (e.g. "C:") and
 * exposes it as a directory symlink under %TEMP%; *out_exposed is that path
 * (caller frees). Honors commit: a dry-run prints the plan and sets *out_exposed
 * to NULL. Returns 0 on success. */
int winvss_create(const char *volume, bool commit, char **out_exposed);

/* Delete the exposed shadow (e.g. "X:"). Honors commit. Returns 0 on success. */
int winvss_release(const char *exposed, bool commit);

#endif /* D2I_WINVSS_H */
