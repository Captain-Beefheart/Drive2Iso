/* Linux host backend (for the AppImage frontend). REAL end-to-end:
 * snapshot/ro-mount a source partition, mksquashfs it, provision a live boot
 * environment, and build an isohybrid BIOS+UEFI ISO with grub-mkrescue. */
#define _DEFAULT_SOURCE  /* declare fsync/lseek/read/write under -std=c11 + glibc */

#include "drive2iso/backend.h"
#include "drive2iso/capture.h"
#include "drive2iso/bootcfg.h"
#include "drive2iso/isobuild.h"
#include "drive2iso/flash.h"
#include "drive2iso/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

/* ---- shared helpers (same idioms as ISO2Drive's backend) ---- */

static char *sh_quote(const char *s) {
    size_t extra = 0;
    for (const char *p = s; *p; ++p) if (*p == '\'') extra += 3;
    char *out = malloc(strlen(s) + extra + 3);
    if (!out) return NULL;
    char *w = out;
    *w++ = '\'';
    for (const char *p = s; *p; ++p) {
        if (*p == '\'') { *w++ = '\''; *w++ = '\\'; *w++ = '\''; *w++ = '\''; }
        else *w++ = *p;
    }
    *w++ = '\'';
    *w = '\0';
    return out;
}

static int run_step(bool commit, const char *cmd) {
    if (!commit) { log_info("(dry-run) %s", cmd); return 0; }
    log_info("+ %s", cmd);
    int rc = system(cmd);
    if (rc != 0) log_err("command failed (status %d): %s", rc, cmd);
    return rc;
}

static bool have_cmd(const char *name) {
    char *c = str_format("command -v %s >/dev/null 2>&1", name);
    if (!c) return false;
    int rc = system(c);
    free(c);
    return rc == 0;
}

/* First line of s, trimmed of trailing newline; edits in place, returns s. */
static char *chomp(char *s) {
    if (!s) return s;
    char *nl = strchr(s, '\n');
    if (nl) *nl = '\0';
    return s;
}

/* run_capture + chomp; NULL/"" -> NULL. */
static char *cap1(const char *cmd) {
    char *o = run_capture(cmd);
    if (!o) return NULL;
    chomp(o);
    if (!*o) { free(o); return NULL; }
    return o;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ---- source discovery ---- */

static int lin_list_sources(void) {
    char *o = run_capture(
        "lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT -e7 2>/dev/null");  /* -e7: skip loop devs */
    if (o && *o) { fputs(o, stdout); free(o); }
    else { free(o); log_warn("lsblk not available"); }
    return 0;
}

static bool dev_is_root_source(const char *dev) {
    char *root = cap1("findmnt -no SOURCE / 2>/dev/null");
    bool r = root && strcmp(root, dev) == 0;
    free(root);
    return r;
}

static int lin_probe_source(const char *device, d2i_source_t *out) {
    memset(out, 0, sizeof *out);
    snprintf(out->device, sizeof out->device, "%s", device);

    bool mp = is_dir(device);   /* caller passed a mountpoint, not a block dev */
    char *q = sh_quote(device);
    if (!q) return -1;

    /* filesystem type */
    char *cmd = mp ? str_format("findmnt -no FSTYPE %s 2>/dev/null", q)
                   : str_format("lsblk -dno FSTYPE %s 2>/dev/null", q);
    char *fs = cmd ? cap1(cmd) : NULL;
    free(cmd);
    if (fs) { snprintf(out->fs, sizeof out->fs, "%s", fs); free(fs); }

    /* size in bytes */
    cmd = mp ? str_format("findmnt -bno SIZE %s 2>/dev/null", q)
             : str_format("lsblk -dbno SIZE %s 2>/dev/null", q);
    char *sz = cmd ? cap1(cmd) : NULL;
    free(cmd);
    if (sz) { out->size_bytes = strtoull(sz, NULL, 10); free(sz); }

    /* used bytes (only meaningful when mounted) */
    if (mp) {
        cmd = str_format("df -B1 --output=used %s 2>/dev/null | tail -1", q);
        char *us = cmd ? cap1(cmd) : NULL;
        free(cmd);
        if (us) { out->used_bytes = strtoull(us, NULL, 10); free(us); }
    }

    /* os heuristic from fs, refined by /etc/os-release if we can read it */
    if (out->fs[0]) {
        if (!strcmp(out->fs, "ntfs") || !strcmp(out->fs, "exfat"))
            snprintf(out->os, sizeof out->os, "windows");
        else if (!strncmp(out->fs, "ext", 3) || !strcmp(out->fs, "btrfs") ||
                 !strcmp(out->fs, "xfs") || !strcmp(out->fs, "f2fs"))
            snprintf(out->os, sizeof out->os, "linux");
    }
    if (mp) {
        char *osr = str_format("%s/etc/os-release", device);
        if (osr && file_exists(osr)) {
            snprintf(out->os, sizeof out->os, "linux");
            char *idc = str_format(". %s >/dev/null 2>&1; . %s; printf '%%s' \"$ID\"", osr, osr);
            char *id = idc ? cap1(idc) : NULL;
            free(idc);
            if (id) { snprintf(out->distro, sizeof out->distro, "%s", id); free(id); }
        }
        free(osr);
    }

    /* firmware + live */
    snprintf(out->firmware, sizeof out->firmware, "%s",
             is_dir("/sys/firmware/efi") ? "UEFI" : "BIOS");
    out->is_live = mp ? (strcmp(device, "/") == 0) : dev_is_root_source(device);

    free(q);
    return 0;
}

static int lin_probe_env(void) {
    static const char *tools[] = { "mksquashfs", "xorriso", "grub-mkrescue",
                                   "mkinitramfs", "unsquashfs", NULL };
    for (int i = 0; tools[i]; ++i)
        log_info("  %-14s %s", tools[i], have_cmd(tools[i]) ? "yes" : "MISSING");
    log_info("  firmware:      %s", is_dir("/sys/firmware/efi") ? "UEFI" : "Legacy BIOS");
    log_info("hint: squashfs-tools + grub-common/grub-pc-bin/grub-efi-amd64-bin + xorriso");
    return 0;
}

/* ---- snapshot / read-only view ---- */

#define SRCMNT "/run/drive2iso/src"

/* Give capture a consistent read-only view. If a mountpoint was passed, use it
 * as-is; otherwise mount the block device read-only under SRCMNT. */
static int lin_snapshot_begin(const d2i_source_t *src, const d2i_target_t *t, char **out_mount) {
    *out_mount = NULL;
    if (is_dir(src->device)) {                 /* already a directory/mountpoint */
        if (t->commit) *out_mount = xstrdup(src->device);
        else log_info("(dry-run) capture from existing mount %s", src->device);
        return 0;
    }
    char *q = sh_quote(src->device);
    if (!q) return -1;
    char *mk = str_format("mkdir -p %s", SRCMNT);
    char *mnt = str_format("mount -o ro %s %s", q, SRCMNT);
    int fail = 0;
    if (mk)  { fail |= (run_step(t->commit, mk)  != 0); free(mk); }
    if (mnt) { fail |= (run_step(t->commit, mnt) != 0); free(mnt); }
    free(q);
    if (!fail && t->commit) *out_mount = xstrdup(SRCMNT);
    return fail ? -1 : 0;
}

static int lin_snapshot_end(const d2i_source_t *src, const char *mount, bool commit) {
    if (!mount) return 0;
    if (is_dir(src->device) && strcmp(mount, SRCMNT) != 0) return 0; /* we didn't mount it */
    char *um = str_format("umount %s", SRCMNT);
    if (um) { run_step(commit, um); free(um); }
    return 0;
}

/* ---- capture payload (mksquashfs) ---- */

static int lin_capture_payload(const d2i_source_t *src, const char *src_mount,
                               const d2i_target_t *t) {
    if (t->commit && !have_cmd("mksquashfs")) {
        log_err("mksquashfs not found (install squashfs-tools)");
        return -1;
    }
    char *qsrc = sh_quote(src_mount);
    char *dst  = str_format("%s/live/filesystem.squashfs", t->stage_dir);
    char *qdst = dst ? sh_quote(dst) : NULL;
    char *excl = capture_excludes_join(src->os, "", " ");   /* quoted, space-sep */
    int rc = -1;
    if (qsrc && qdst && excl) {
        const char *comp = t->comp_level > 0 ? "" : "-comp zstd";
        char *lvl = t->comp_level > 0 ? str_format("-comp zstd -Xcompression-level %d", t->comp_level)
                                      : xstrdup(comp);
        char *cmd = str_format("mksquashfs %s %s -noappend %s -wildcards -e %s",
                               qsrc, qdst, lvl ? lvl : "", excl);
        if (cmd) { rc = run_step(t->commit, cmd); free(cmd); }
        free(lvl);
    }
    free(qsrc); free(dst); free(qdst); free(excl);
    return rc;
}

/* ---- boot environment (kernel + live initrd + grub menu) ---- */

static int lin_provision_bootenv(const d2i_source_t *src, const d2i_target_t *t) {
    const char *root = is_dir(src->device) ? SRCMNT : SRCMNT; /* capture used SRCMNT or the mount */
    const char *srcroot = is_dir(src->device) ? src->device : SRCMNT;
    (void)root;

    /* newest kernel + initrd from the source /boot */
    char *kcmd = str_format("ls -1 %s/boot/vmlinuz-* 2>/dev/null | sort -V | tail -1", srcroot);
    char *icmd = str_format("ls -1 %s/boot/initrd.img-* %s/boot/initramfs-* 2>/dev/null | sort -V | tail -1",
                            srcroot, srcroot);
    char *kern = kcmd ? cap1(kcmd) : NULL;
    char *init = icmd ? cap1(icmd) : NULL;
    free(kcmd); free(icmd);

    int fail = 0;
    if (kern) {
        char *qk = sh_quote(kern);
        char *cp = str_format("cp -f %s %s/live/vmlinuz", qk, t->stage_dir);
        if (cp) { fail |= (run_step(t->commit, cp) != 0); free(cp); }
        free(qk);
    } else {
        log_warn("no vmlinuz found under %s/boot — copy a kernel into %s/live/vmlinuz manually",
                 srcroot, t->stage_dir);
        fail = 1;
    }

    /* A LIVE initrd (with the live-boot/overlay layer) is what makes the squashfs
     * boot. If the source already has one we copy it; otherwise emit the exact
     * regen command rather than silently shipping a non-live initrd. */
    if (init) {
        char *qi = sh_quote(init);
        char *cp = str_format("cp -f %s %s/live/initrd.img", qi, t->stage_dir);
        if (cp) { fail |= (run_step(t->commit, cp) != 0); free(cp); }
        free(qi);
        log_warn("copied the source initrd; it must contain the 'live-boot' layer to boot the squashfs.");
        log_warn("regen if needed:  chroot %s update-initramfs -c -k all   (after installing live-boot)", srcroot);
    } else {
        log_warn("no initrd found; build one with live-boot, e.g.:");
        log_warn("  chroot %s sh -c 'apt-get install -y live-boot && update-initramfs -c -k all'", srcroot);
        fail = 1;
    }

    /* GRUB menu (BIOS+UEFI) via the portable generator. */
    const live_profile_t *prof = bootcfg_profile(src->distro);
    if (!prof) prof = bootcfg_generic();
    if (bootcfg_write_linux(t->stage_dir, "DRIVE2ISO", "vmlinuz", "initrd.img",
                            prof, t->commit) != 0)
        fail = 1;

    return fail ? -1 : 0;
}

/* ---- build the ISO ---- */

static int lin_build_iso(const d2i_source_t *src, const d2i_target_t *t) {
    (void)src;
    iso_tools_t tools = {
        .have_grub_mkrescue = have_cmd("grub-mkrescue"),
        .have_xorriso       = have_cmd("xorriso"),
        .have_oscdimg       = false,
    };
    char *cmd = isobuild_command(t->stage_dir, t->out_iso, "DRIVE2ISO",
                                 "linux", t->isohybrid, &tools);
    if (!cmd) {
        log_err("no ISO builder found — install grub-common (grub-mkrescue) + xorriso");
        return -1;
    }
    int rc = run_step(t->commit, cmd);
    free(cmd);
    if (rc == 0 && t->commit) log_info("built %s", t->out_iso);
    return rc;
}

/* ---- bootable USB (raw flash) — reused from ISO2Drive ---- */

typedef struct { int fd; } ldev;

static long ldev_write(void *c, const void *buf, size_t len) {
    int fd = ((ldev *)c)->fd;
    const char *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return (long)off;
}
static long ldev_read(void *c, void *buf, size_t len) {
    int fd = ((ldev *)c)->fd;
    char *p = buf;
    size_t off = 0;
    while (off < len) {
        ssize_t r = read(fd, p + off, len - off);
        if (r <= 0) return -1;
        off += (size_t)r;
    }
    return (long)off;
}
static int  ldev_rewind(void *c) { return lseek(((ldev *)c)->fd, 0, SEEK_SET) == (off_t)-1 ? -1 : 0; }
static void ldev_close(void *c) {
    ldev *d = c;
    if (d) { if (d->fd >= 0) { fsync(d->fd); close(d->fd); } free(d); }
}

static int disk_is_removable(const char *device) {
    const char *b = strrchr(device, '/');
    b = b ? b + 1 : device;
    char *cmd = str_format("cat /sys/block/%s/removable 2>/dev/null", b);
    if (!cmd) return -1;
    char *o = run_capture(cmd);
    free(cmd);
    int r = -1;
    if (o) { if (o[0] == '1') r = 1; else if (o[0] == '0') r = 0; free(o); }
    return r;
}

static int lin_write_usb(const char *iso, const char *device,
                         bool commit, bool verify, bool force) {
    if (dev_is_root_source(device)) {
        log_err("refusing: %s is the running system disk", device);
        return -1;
    }
    int rem = disk_is_removable(device);
    log_info("target: %s  removable=%s", device, rem == 1 ? "yes" : rem == 0 ? "no" : "?");

    if (!commit) {
        log_info("(dry-run) would raw-write %s -> %s (dd-style)%s", iso, device, verify ? " + verify" : "");
        if (rem == 0) log_warn("%s is NOT removable; --commit will need --force", device);
        log_info("re-run as root with --commit to flash");
        return 0;
    }
    if (rem == 0 && !force) {
        log_err("%s is not removable; pass --force to override", device);
        return -1;
    }

    int fd = open(device, O_RDWR);
    if (fd < 0) { log_err("cannot open %s (need root?)", device); return -1; }
    ldev *d = malloc(sizeof *d);
    if (!d) { close(fd); return -1; }
    d->fd = fd;

    flash_dev_t dev = {
        .ctx = d, .align = 512,
        .write = ldev_write, .read = ldev_read, .rewind = ldev_rewind, .close = ldev_close,
    };
    log_warn("raw-writing to %s — all existing data is destroyed", device);
    int rc = flash_iso_to_dev(iso, &dev, verify);
    dev.close(dev.ctx);
    return rc;
}

static int lin_list_disks(void) {
    char *o = run_capture("lsblk -d -o NAME,SIZE,TYPE,TRAN,HOTPLUG,MODEL 2>/dev/null");
    if (o && *o) { fputs(o, stdout); free(o); }
    else { free(o); log_warn("lsblk not available"); }
    return 0;
}

static const d2i_backend_t g_backend = {
    .name              = "linux",
    .list_sources      = lin_list_sources,
    .probe_source      = lin_probe_source,
    .probe_env         = lin_probe_env,
    .snapshot_begin    = lin_snapshot_begin,
    .snapshot_end      = lin_snapshot_end,
    .capture_payload   = lin_capture_payload,
    .provision_bootenv = lin_provision_bootenv,
    .build_iso         = lin_build_iso,
    .write_usb         = lin_write_usb,
    .list_disks        = lin_list_disks,
};
const d2i_backend_t *backend_get(void) { return &g_backend; }
