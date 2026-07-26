/* Windows host backend (for the .exe frontend). Capture is REAL: a VSS snapshot
 * (via diskshadow) + a WIM capture (wimlib/DISM). The bootable-ISO assembly is
 * GATED on the host Windows ADK (WinPE + oscdimg), exactly the way ISO2Drive
 * gates its BIOS-GRUB path on bundled binaries — never a silent partial.
 *
 * Note: a truly *live* Windows-from-USB (Windows-To-Go) is deprecated and out
 * of scope; the artifact here is WinPE + captured WIM install/restore media. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "drive2iso/backend.h"
#include "drive2iso/capture.h"
#include "drive2iso/isobuild.h"
#include "drive2iso/flash.h"
#include "drive2iso/util.h"
#include "winutil.h"
#include "winusb.h"
#include "winvss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/* ---- helpers ---- */

static int run_step(bool commit, const char *cmd) {
    if (!commit) { log_info("(dry-run) %s", cmd); return 0; }
    log_info("+ %s", cmd);
    int rc = system(cmd);
    if (rc != 0) log_err("command failed (status %d): %s", rc, cmd);
    return rc;
}

static bool where_cmd(const char *name) {
    char *c = str_format("where %s >nul 2>nul", name);
    if (!c) return false;
    int rc = system(c);
    free(c);
    return rc == 0;
}

/* Copy with '/' turned into '\\' for native tools (oscdimg/xcopy/dism). */
static char *winpath(const char *p) {
    char *d = xstrdup(p);
    if (d) for (char *q = d; *q; ++q) if (*q == '/') *q = '\\';
    return d;
}

/* First drive-letter in a "C:", "c", or "C:\\" style argument. */
static char norm_letter(const char *device) {
    for (const char *p = device; *p; ++p)
        if (isalpha((unsigned char)*p)) return (char)toupper((unsigned char)*p);
    return 0;
}

/* Nth line of a captured multi-line string into buf (0-based). */
static void nth_line(const char *s, int n, char *buf, size_t bufsz) {
    buf[0] = '\0';
    for (int i = 0; s && i < n; ++i) { s = strchr(s, '\n'); if (s) ++s; else return; }
    if (!s) return;
    size_t j = 0;
    while (*s && *s != '\r' && *s != '\n' && j + 1 < bufsz) buf[j++] = *s++;
    buf[j] = '\0';
}

/* ---- source discovery ---- */

static int win_list_sources(void) {
    char *o = run_capture(
        "powershell -NoProfile -Command "
        "\"Get-Volume | Where-Object DriveLetter | "
        "Format-Table DriveLetter,FileSystemLabel,FileSystem,"
        "@{N='SizeMB';E={[int]($_.Size/1MB)}},"
        "@{N='FreeMB';E={[int]($_.SizeRemaining/1MB)}} -Auto\" 2>nul");
    if (o && *o) { fputs(o, stdout); free(o); }
    else { free(o); log_warn("could not enumerate volumes (PowerShell?)"); }
    return 0;
}

static int win_probe_source(const char *device, d2i_source_t *out) {
    memset(out, 0, sizeof *out);
    char L = norm_letter(device);
    if (!L) { log_err("expected a drive letter like C:"); return -1; }
    snprintf(out->device, sizeof out->device, "%c:", L);

    char *cmd = str_format(
        "powershell -NoProfile -Command "
        "\"$v=Get-Volume -DriveLetter %c -ErrorAction SilentlyContinue; "
        "Write-Output $v.FileSystem; Write-Output $v.Size; "
        "Write-Output ($v.Size - $v.SizeRemaining)\" 2>nul", L);
    char *o = cmd ? run_capture(cmd) : NULL;
    free(cmd);
    if (o) {
        char fs[16], sz[32], us[32];
        nth_line(o, 0, fs, sizeof fs);
        nth_line(o, 1, sz, sizeof sz);
        nth_line(o, 2, us, sizeof us);
        if (fs[0]) snprintf(out->fs, sizeof out->fs, "%s", fs);
        out->size_bytes = strtoull(sz, NULL, 10);
        out->used_bytes = strtoull(us, NULL, 10);
        free(o);
    }

    /* os heuristic: a \Windows\System32 marks a Windows volume. */
    char *marker = str_format("%c:\\Windows\\System32\\ntoskrnl.exe", L);
    if (marker && file_exists(marker)) snprintf(out->os, sizeof out->os, "windows");
    else if (out->fs[0] && (!strcmp(out->fs, "NTFS") || !strcmp(out->fs, "ReFS")))
        snprintf(out->os, sizeof out->os, "windows");
    free(marker);

    snprintf(out->firmware, sizeof out->firmware, "%s", win_firmware_name(win_firmware_type()));
    const char *sd = getenv("SystemDrive");
    out->is_live = (sd && toupper((unsigned char)sd[0]) == L);
    return 0;
}

static int win_probe_env(void) {
    static const char *tools[] = { "wimlib-imagex", "dism", "oscdimg", "xorriso", NULL };
    for (int i = 0; tools[i]; ++i)
        log_info("  %-14s %s", tools[i], where_cmd(tools[i]) ? "yes" : "MISSING");
    log_info("  firmware:      %s", win_firmware_name(win_firmware_type()));
    log_info("  elevated:      %s", win_is_elevated() ? "yes" : "NO (capture/VSS need admin)");
    log_info("hint: oscdimg + WinPE come from the Windows ADK + 'Windows PE add-on'.");
    log_info("      run from the 'Deployment and Imaging Tools Environment' prompt.");
    return 0;
}

/* ---- snapshot (VSS via diskshadow) ---- */

static int win_snapshot_begin(const d2i_source_t *src, const d2i_target_t *t, char **out_mount) {
    return winvss_create(src->device, t->commit, out_mount);
}
static int win_snapshot_end(const d2i_source_t *src, const char *mount, bool commit) {
    (void)src;
    return winvss_release(mount, commit);
}

/* ---- capture payload (WIM) ---- */

static int win_capture_payload(const d2i_source_t *src, const char *src_mount,
                               const d2i_target_t *t) {
    char *work  = winpath(t->work_dir);
    char *stage = winpath(t->stage_dir);
    char *cfg   = work  ? str_format("%s\\wimscript.ini", work)   : NULL;
    char *wim   = stage ? str_format("%s\\sources\\install.wim", stage) : NULL;
    char *capdir = str_format("%s\\", src_mount ? src_mount : src->device);
    int rc = -1;
    if (cfg && wim && capdir) {
        capture_write_wim_config(cfg, t->commit);
        bool wimlib = where_cmd("wimlib-imagex");
        bool dism   = where_cmd("dism");
        if (!wimlib && !dism && t->commit) {
            log_err("need wimlib-imagex or DISM to capture the volume");
        } else {
            char *cmd;
            if (dism && !wimlib)
                cmd = str_format(
                    "dism /Capture-Image /ImageFile:\"%s\" /CaptureDir:%s "
                    "/Name:\"Drive2Iso capture\" /ConfigFile:\"%s\"", wim, capdir, cfg);
            else
                cmd = str_format(
                    "wimlib-imagex capture \"%s\" \"%s\" \"Drive2Iso capture\" "
                    "--config=\"%s\" --compress=LZX", capdir, wim, cfg);
            if (cmd) { rc = run_step(t->commit, cmd); free(cmd); }
        }
    }
    if (rc == 0 && t->commit)
        log_info("note: to flash onto FAT32 USB, split >4GiB WIM "
                 "(wimlib-imagex split / dism /Split-Image)");
    free(work); free(stage); free(cfg); free(wim); free(capdir);
    return rc;
}

/* ---- boot environment (WinPE from the ADK) ---- */

static int win_provision_bootenv(const d2i_source_t *src, const d2i_target_t *t) {
    (void)src;
    bool has_oscdimg = where_cmd("oscdimg");
    if (!has_oscdimg) {
        log_warn("Windows ADK (oscdimg) not found — install the ADK + 'Windows PE add-on',");
        log_warn("then run Drive2Iso from the 'Deployment and Imaging Tools Environment'.");
        if (t->commit) { log_err("cannot build bootable Windows media without the ADK"); return -1; }
    }
    char *work  = winpath(t->work_dir);
    char *stage = winpath(t->stage_dir);
    char *winpe = work ? str_format("%s\\winpe", work) : NULL;
    int fail = 0;
    if (winpe && stage) {
        /* copype lays down a WinPE 'media' tree (bootmgr, boot/, efi/, sources/
         * boot.wim). We overlay it onto the stage that already holds install.wim. */
        char *c1 = str_format("copype amd64 \"%s\"", winpe);
        char *c2 = str_format("xcopy /e /h /y \"%s\\media\\*\" \"%s\\\"", winpe, stage);
        if (c1) { fail |= (run_step(t->commit, c1) != 0); free(c1); }
        if (c2) { fail |= (run_step(t->commit, c2) != 0); free(c2); }
        log_info("WinPE will boot and can apply sources\\install.wim to a target disk");
    }
    free(work); free(stage); free(winpe);
    return (t->commit && fail) ? -1 : 0;
}

/* ---- build the ISO ---- */

static int win_build_iso(const d2i_source_t *src, const d2i_target_t *t) {
    (void)src;
    iso_tools_t tools = {
        .have_grub_mkrescue = false,
        .have_xorriso       = where_cmd("xorriso"),
        .have_oscdimg       = where_cmd("oscdimg"),
    };
    if (!tools.have_oscdimg && !tools.have_xorriso) {
        if (t->commit) { log_err("no ISO builder found — install the ADK (oscdimg) or xorriso"); return -1; }
        tools.have_oscdimg = true; /* show the intended oscdimg plan in dry-run */
    }
    char *stage = winpath(t->stage_dir);
    char *out   = winpath(t->out_iso);
    char *cmd = isobuild_command(stage ? stage : t->stage_dir, out ? out : t->out_iso,
                                 "DRIVE2ISO", "windows", t->isohybrid, &tools);
    int rc = -1;
    if (cmd) { rc = run_step(t->commit, cmd); free(cmd); }
    else log_err("could not construct an imager command");
    if (rc == 0 && t->commit) log_info("built %s", t->out_iso);
    free(stage); free(out);
    return rc;
}

/* ---- bootable USB (raw flash) — reuses winusb + the shared flasher ---- */

static long wu_write(void *c, const void *b, size_t n) { return winusb_write(c, b, n); }
static long wu_read (void *c, void *b, size_t n)       { return winusb_read(c, b, n); }
static int  wu_rewind(void *c)                         { return winusb_rewind(c); }
static void wu_close(void *c)                          { winusb_close(c); }

static int parse_drive(const char *s) {
    const char *p = s + strlen(s);
    while (p > s && isdigit((unsigned char)p[-1])) --p;
    if (!*p) return -1;
    return atoi(p);
}

static int win_write_usb(const char *iso, const char *device,
                         bool commit, bool verify, bool force) {
    int drive = parse_drive(device);
    if (drive < 0) {
        log_err("bad drive '%s' (use a number, PhysicalDriveN, or \\\\.\\PhysicalDriveN)", device);
        return -1;
    }
    if (winusb_hosts_system(drive) == 1) {
        log_err("refusing: PhysicalDrive%d hosts the Windows system volume", drive);
        return -1;
    }
    int rem = winusb_is_removable(drive);
    uint64_t sz = winusb_size(drive);
    log_info("target: PhysicalDrive%d  %llu MiB  removable=%s", drive,
             (unsigned long long)(sz >> 20), rem == 1 ? "yes" : rem == 0 ? "no" : "?");

    if (!commit) {
        log_info("(dry-run) would raw-write %s -> PhysicalDrive%d%s", iso, drive, verify ? " + verify" : "");
        if (rem == 0) log_warn("PhysicalDrive%d is NOT removable; --commit will need --force", drive);
        log_info("re-run from an elevated prompt with --commit to flash");
        return 0;
    }
    if (rem == 0 && !force) {
        log_err("PhysicalDrive%d is not removable; pass --force to override", drive);
        return -1;
    }

    char *err = NULL;
    winusb_dev *d = winusb_open(drive, &err);
    if (!d) { log_err("%s", err ? err : "open failed"); free(err); return -1; }

    flash_dev_t dev = {
        .ctx = d, .align = 512,
        .write = wu_write, .read = wu_read, .rewind = wu_rewind, .close = wu_close,
    };
    log_warn("raw-writing to PhysicalDrive%d — all existing data is destroyed", drive);
    int rc = flash_iso_to_dev(iso, &dev, verify);
    dev.close(dev.ctx);
    return rc;
}

static int win_list_disks(void) { winusb_list(); return 0; }

static const d2i_backend_t g_backend = {
    .name              = "windows",
    .list_sources      = win_list_sources,
    .probe_source      = win_probe_source,
    .probe_env         = win_probe_env,
    .snapshot_begin    = win_snapshot_begin,
    .snapshot_end      = win_snapshot_end,
    .capture_payload   = win_capture_payload,
    .provision_bootenv = win_provision_bootenv,
    .build_iso         = win_build_iso,
    .write_usb         = win_write_usb,
    .list_disks        = win_list_disks,
};
const d2i_backend_t *backend_get(void) { return &g_backend; }
