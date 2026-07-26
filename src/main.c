/* Drive2Iso CLI frontend. Thin: it wires the host backend to the portable core
 * (capture rules + boot-menu / imager-command generation) and presents an
 * Etcher-style three-step flow: SOURCE -> CAPTURE -> IMAGE. */
#include "drive2iso/backend.h"
#include "drive2iso/capture.h"
#include "drive2iso/ui.h"
#include "drive2iso/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIB(x) ((unsigned long long)((x) >> 20))

static char *default_workdir(void) {
#ifdef _WIN32
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = "C:\\Temp";
    return str_format("%s\\drive2iso", tmp);
#else
    return xstrdup("/var/tmp/drive2iso");
#endif
}

static int cmd_list_parts(const d2i_backend_t *b) {
    ui_banner();
    return b->list_sources();
}

static int cmd_probe(const d2i_backend_t *b, const char *device) {
    d2i_source_t src;
    memset(&src, 0, sizeof src);
    if (b->probe_source(device, &src) != 0) { log_err("cannot probe %s", device); return 1; }
    log_info("device:   %s", src.device[0] ? src.device : device);
    log_info("fs:       %s", src.fs[0] ? src.fs : "?");
    log_info("os:       %s%s%s", src.os[0] ? src.os : "?",
             src.distro[0] ? " / " : "", src.distro);
    log_info("size:     %llu MiB", MIB(src.size_bytes));
    log_info("used:     %llu MiB", MIB(src.used_bytes));
    log_info("firmware: %s", src.firmware[0] ? src.firmware : "?");
    log_info("live:     %s", src.is_live ? "YES (running system volume)" : "no");
    return 0;
}

static int cmd_doctor(const d2i_backend_t *b) {
    ui_banner();
    log_info("backend: %s", b->name);
    return b->probe_env();
}

static int cmd_capture(const d2i_backend_t *b, int argc, char **argv) {
    const char *device  = argv[2];
    const char *out_iso = argv[3];

    d2i_target_t t = {0};
    t.out_iso   = out_iso;
    t.do_uefi   = true;
    t.do_bios   = true;
    t.isohybrid = true;
    const char *work_opt = NULL;
    for (int i = 4; i < argc; ++i) {
        if      (!strcmp(argv[i], "--commit"))        t.commit = true;
        else if (!strcmp(argv[i], "--no-uefi"))       t.do_uefi = false;
        else if (!strcmp(argv[i], "--no-bios"))       t.do_bios = false;
        else if (!strcmp(argv[i], "--no-isohybrid"))  t.isohybrid = false;
        else if (!strcmp(argv[i], "--comp") && i + 1 < argc) t.comp_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) work_opt = argv[++i];
        else log_warn("ignoring unknown option: %s", argv[i]);
    }

    char *work  = work_opt ? xstrdup(work_opt) : default_workdir();
    char *stage = str_format("%s/iso", work);
    if (!work || !stage) { free(work); free(stage); return 1; }
    t.work_dir  = work;
    t.stage_dir = stage;

    ui_banner();

    /* (1) SOURCE — probe the partition. */
    ui_step(1, 1, "SOURCE", device);
    d2i_source_t src;
    memset(&src, 0, sizeof src);
    if (b->probe_source(device, &src) != 0) {
        log_err("cannot probe %s", device);
        free(work); free(stage); return 1;
    }
    log_info("  fs=%s  os=%s%s%s  size=%llu MiB  used=%llu MiB  firmware=%s  live=%s",
             src.fs[0] ? src.fs : "?", src.os[0] ? src.os : "?",
             src.distro[0] ? "/" : "", src.distro,
             MIB(src.size_bytes), MIB(src.used_bytes),
             src.firmware[0] ? src.firmware : "?", src.is_live ? "YES" : "no");
    if (src.is_live)
        log_warn("  %s is the RUNNING system volume; a snapshot is used for consistency", device);

    capture_prepare_tree(stage, src.os, t.commit);

    /* (2) CAPTURE — snapshot the source, then squash/WIM the payload. */
    ui_step(2, t.commit ? 1 : 0, "CAPTURE",
            t.commit ? "snapshot + payload" : "dry-run (pass --commit to build)");
    char *mount = NULL;
    int rc = b->snapshot_begin(&src, &t, &mount);
    if (rc == 0)
        rc = b->capture_payload(&src, mount ? mount : device, &t);

    /* (3) IMAGE — boot environment, then run the imager. */
    if (rc == 0) {
        ui_step(3, t.commit ? 1 : 0, "IMAGE",
                t.commit ? "boot env + build ISO" : "dry-run (pass --commit to build)");
        rc = b->provision_bootenv(&src, &t);
        if (rc == 0) rc = b->build_iso(&src, &t);
    }

    b->snapshot_end(&src, mount, t.commit);
    free(mount);

    if (rc == 0 && t.commit)
        log_info("done — %s", out_iso);
    else if (rc == 0)
        log_info("dry-run complete — re-run with --commit to build %s", out_iso);
    else
        log_err("capture failed");

    free(work); free(stage);
    return rc ? 1 : 0;
}

static int cmd_write_usb(const d2i_backend_t *b, int argc, char **argv) {
    const char *iso    = argv[2];
    const char *device = argv[3];
    bool commit = false, verify = false, force = false;
    for (int i = 4; i < argc; ++i) {
        if      (!strcmp(argv[i], "--commit")) commit = true;
        else if (!strcmp(argv[i], "--verify")) verify = true;
        else if (!strcmp(argv[i], "--force"))  force = true;
        else log_warn("ignoring unknown option: %s", argv[i]);
    }
    ui_banner();
    ui_step(1, 1, "SOURCE", iso);
    ui_step(2, 1, "CAPTURE", "(already an ISO)");
    ui_step(3, commit ? 1 : 0, "IMAGE",
            commit ? "raw write (dd-style)" : "dry-run (pass --commit to flash)");
    return b->write_usb(iso, device, commit, verify, force) ? 1 : 0;
}

static void usage(void) {
    ui_banner();
    printf(
        "  usage:\n"
        "    drive2iso list-parts                         list candidate source partitions\n"
        "    drive2iso probe <device>                     inspect a partition (fs/os/size/live)\n"
        "    drive2iso doctor                             report tools + firmware mode\n"
        "    drive2iso capture <device> <out.iso> [opts]  capture a partition into a bootable ISO\n"
        "    drive2iso write-usb <iso> <device> [opts]    raw dd-style flash of a produced ISO\n"
        "    drive2iso help\n"
        "\n"
        "  <device>: Linux /dev/sdXN (or a mountpoint); Windows a drive letter (C:) or volume.\n"
        "  capture is a DRY-RUN until --commit (prints the exact mksquashfs/wimlib/xorriso plan).\n"
        "\n"
        "  capture options:\n"
        "    --commit          actually build (default is a dry-run preview)\n"
        "    --no-uefi / --no-bios / --no-isohybrid\n"
        "    --comp <level>    squashfs/wim compression knob\n"
        "    --work <dir>      staging/scratch dir (default: temp/drive2iso)\n"
        "\n"
        "  write-usb options:  --commit  --verify  --force (allow non-removable targets)\n"
        "  write-usb + --commit ERASE the target (root/admin); refuses the system disk.\n");
}

int main(int argc, char **argv) {
    ui_init();
    const d2i_backend_t *b = backend_get();
    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "list-parts") && argc == 2) return cmd_list_parts(b);
    if (!strcmp(argv[1], "probe")      && argc == 3) return cmd_probe(b, argv[2]);
    if (!strcmp(argv[1], "doctor")     && argc == 2) return cmd_doctor(b);
    if (!strcmp(argv[1], "capture")    && argc >= 4) return cmd_capture(b, argc, argv);
    if (!strcmp(argv[1], "write-usb")  && argc >= 4) return cmd_write_usb(b, argc, argv);
    if (!strcmp(argv[1], "help")) { usage(); return 0; }
    usage();
    return 1;
}
