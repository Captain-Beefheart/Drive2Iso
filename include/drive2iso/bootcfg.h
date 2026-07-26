#ifndef D2I_BOOTCFG_H
#define D2I_BOOTCFG_H

#include <stdbool.h>

/* The fiddly per-distro live-boot logic, in exactly one place (mirrors
 * ISO2Drive's profile.c + grubcfg.c). A profile says how a captured rootfs of a
 * given family is told to boot LIVE off the ISO: which initrd "live layer" to
 * build, the kernel cmdline that triggers the live overlay, and where the
 * squashfs must sit in the ISO tree (this differs per family).
 *
 * This table is a STARTING POINT — validate it against real systems. The
 * debian/ubuntu/generic rows (live-boot, /live/filesystem.squashfs) are the
 * fully-wired v1 path; fedora (dracut) and arch (mkinitcpio) rows are provided
 * for their native layouts and need their own initrd tooling to be complete. */
typedef struct {
    const char *family;      /* "debian" | "ubuntu" | "fedora" | "arch" | "generic" */
    const char *live_layer;  /* initrd live module: "live-boot" | "dracut" | "mkinitcpio" */
    const char *cmdline;     /* kernel cmdline; may contain one %s for the volume label */
    const char *squash_rel;  /* squashfs path inside the ISO tree, e.g. "live/filesystem.squashfs" */
} live_profile_t;

/* Match a family by case-insensitive substring on the distro id (e.g. "ubuntu",
 * "debian", "fedora", "arch", "manjaro"). NULL if unknown. */
const live_profile_t *bootcfg_profile(const char *distro);

/* The generic fallback profile (live-boot, /live/filesystem.squashfs). */
const live_profile_t *bootcfg_generic(void);

/* Emit the GRUB boot menu for a Linux live ISO into stage_dir
 * (<stage>/boot/grub/grub.cfg). GRUB covers BOTH BIOS (via grub-mkrescue's
 * El Torito image) and UEFI, so no separate isolinux menu is needed. `label` is
 * the ISO volume id; `kernel`/`initrd` are the basenames placed under /live.
 * Honors commit (dry-run logs the path + contents preview). Returns 0 on ok. */
int bootcfg_write_linux(const char *stage_dir, const char *label,
                        const char *kernel, const char *initrd,
                        const live_profile_t *prof, bool commit);

#endif /* D2I_BOOTCFG_H */
