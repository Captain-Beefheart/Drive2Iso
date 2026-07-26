#ifndef D2I_BACKEND_H
#define D2I_BACKEND_H

#include <stdbool.h>
#include <stdint.h>

/* Drive2Iso is the inverse of ISO2Drive: it captures a live/installed partition
 * and emits a bootable ISO. The portable core (capture rules, boot-menu and
 * imager-command generation) never touches platform code; a host backend
 * supplies the platform half (enumerate/probe volumes, snapshot, capture the
 * payload, provision the boot environment, run the imager). */

/* SOURCE — a partition/volume to capture. Filled by probe_source(). */
typedef struct {
    char     device[256];  /* /dev/sda2 | C: | \\?\Volume{...} */
    char     fs[16];       /* ext4 / btrfs / xfs / ntfs / vfat / ... */
    char     os[16];       /* "linux" | "windows" | "" (unknown) */
    char     distro[64];   /* best-effort family id: ubuntu/debian/fedora/arch/... */
    char     firmware[8];  /* "UEFI" | "BIOS" | "" — how the host itself booted */
    uint64_t size_bytes;   /* partition size */
    uint64_t used_bytes;   /* used space (sizes the staging area); 0 if unknown */
    bool     is_live;      /* is this the running system volume? */
} d2i_source_t;

/* TARGET — how and where to build the ISO. */
typedef struct {
    const char *out_iso;    /* final image path */
    const char *stage_dir;  /* ISO tree is assembled here */
    const char *work_dir;   /* scratch: snapshot mount, wim split, kernel copy, ... */
    bool  do_uefi;
    bool  do_bios;
    bool  isohybrid;        /* apply isohybrid MBR/GPT so a dd to USB/disk boots */
    int   comp_level;       /* squashfs/wim compression knob; 0 = tool default */
    bool  commit;           /* false = dry-run: print the plan, change nothing */
} d2i_target_t;

/* A host backend supplies the platform-specific half of the pipeline. */
typedef struct d2i_backend {
    const char *name;

    /* --- source discovery --- */
    int (*list_sources)(void);                              /* print candidate volumes */
    int (*probe_source)(const char *device, d2i_source_t *out);
    int (*probe_env)(void);                                 /* report tool availability */

    /* --- consistent point-in-time view of a (possibly live) source --- */
    /* On commit, sets *out_mount to a read-only view (snapshot mount, or the
     * source itself when a snapshot is not possible); caller frees. Dry-run
     * prints the plan and sets *out_mount to NULL. */
    int (*snapshot_begin)(const d2i_source_t *src, const d2i_target_t *t, char **out_mount);
    int (*snapshot_end)(const d2i_source_t *src, const char *mount, bool commit);

    /* --- capture the source contents into the staging tree's payload --- */
    /* Linux  -> <stage>/live/filesystem.squashfs (mksquashfs)
     * Windows -> <stage>/sources/install.wim     (wimlib/DISM), split if needed */
    int (*capture_payload)(const d2i_source_t *src, const char *src_mount,
                           const d2i_target_t *t);

    /* --- provision the boot environment into the staging tree --- */
    /* Linux  -> copy vmlinuz + build a live initrd, write the boot menu
     * Windows -> stage WinPE boot.wim + bootmgr/EFI from the host ADK */
    int (*provision_bootenv)(const d2i_source_t *src, const d2i_target_t *t);

    /* --- emit the ISO (grub-mkrescue / xorriso / oscdimg) --- */
    int (*build_iso)(const d2i_source_t *src, const d2i_target_t *t);

    /* --- reuse: raw dd-style flash of the produced ISO onto a whole device --- */
    int (*write_usb)(const char *iso, const char *device,
                     bool commit, bool verify, bool force);
    int (*list_disks)(void);   /* print candidate target devices */
} d2i_backend_t;

const d2i_backend_t *backend_get(void);

#endif /* D2I_BACKEND_H */
