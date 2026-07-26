/* Portable capture policy: exclude rules + ISO staging-tree layout. The actual
 * copy (mksquashfs / wimlib) lives in the backends; this is the shared contract
 * both must agree on. */
#include "drive2iso/capture.h"
#include "drive2iso/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Linux: mountpoints, scratch, swap and caches — relative to the captured root.
 * The live ISO carries its own /boot bits, so the source's boot/efi is dropped. */
static const char *const lin_excludes[] = {
    "proc", "sys", "dev", "run", "tmp", "mnt", "media",
    "lost+found", "var/tmp", "var/cache/apt/archives", "var/lib/docker",
    "swapfile", "swap.img", "boot/efi",
    NULL
};

/* Windows: pagefiles, hibernation, recycle/restore, offline caches. Paths are
 * backslash-relative to the volume root (wimlib/DISM WIMSCRIPT style). */
static const char *const win_excludes[] = {
    "pagefile.sys", "hiberfil.sys", "swapfile.sys",
    "System Volume Information", "$Recycle.Bin", "Recovery",
    "Windows\\Temp", "Windows\\CSC",
    NULL
};

const char *const *capture_excludes(const char *os) {
    if (os && strcmp(os, "windows") == 0) return win_excludes;
    return lin_excludes;
}

char *capture_excludes_join(const char *os, const char *prefix, const char *sep) {
    const char *const *ex = capture_excludes(os);
    if (!prefix) prefix = "";
    if (!sep)    sep = " ";
    size_t total = 1;
    for (int i = 0; ex[i]; ++i)
        total += strlen(sep) + strlen(prefix) + strlen(ex[i]) + 3; /* +quotes/nul */
    char *out = malloc(total);
    if (!out) return NULL;
    size_t off = 0;
    out[0] = '\0';
    for (int i = 0; ex[i]; ++i) {
        int n = snprintf(out + off, total - off, "%s%s\"%s\"",
                         i ? sep : "", prefix, ex[i]);
        if (n < 0) break;
        off += (size_t)n;
    }
    return out;
}

/* Best-effort nested mkdir (each level in turn). */
static void ensure_path(const char *stage, const char *rel, bool commit) {
    char *full = str_format("%s/%s", stage, rel);
    if (!full) return;
    if (commit) ensure_dir(full);
    else        log_info("(dry-run) mkdir %s", full);
    free(full);
}

int capture_prepare_tree(const char *stage_dir, const char *os, bool commit) {
    if (!stage_dir) return -1;
    if (commit) ensure_dir(stage_dir);
    else        log_info("(dry-run) mkdir %s", stage_dir);

    if (os && strcmp(os, "windows") == 0) {
        static const char *win[] = { "sources", "boot", "efi", "efi/microsoft",
                                     "efi/microsoft/boot", NULL };
        for (int i = 0; win[i]; ++i) ensure_path(stage_dir, win[i], commit);
    } else {
        static const char *lin[] = { "live", "boot", "boot/grub",
                                     "EFI", "EFI/BOOT", NULL };
        for (int i = 0; lin[i]; ++i) ensure_path(stage_dir, lin[i], commit);
    }
    return 0;
}

int capture_write_wim_config(const char *path, bool commit) {
    /* WIMSCRIPT understood by wimlib-imagex --config and DISM /ConfigFile. */
    char buf[1024];
    size_t off = 0;
    off += (size_t)snprintf(buf + off, sizeof buf - off, "[ExclusionList]\n");
    for (int i = 0; win_excludes[i]; ++i)
        off += (size_t)snprintf(buf + off, sizeof buf - off, "\\%s\n", win_excludes[i]);
    snprintf(buf + off, sizeof buf - off,
             "[CompressionExclusionList]\n*.mp3\n*.zip\n*.cab\n*.wim\n");
    if (!commit) { log_info("(dry-run) write wim capture config -> %s", path); return 0; }
    return write_file(path, buf);
}
