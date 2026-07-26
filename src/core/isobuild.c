/* Construct the imager command that turns a staging tree into a bootable ISO.
 * Pure string building; the backend runs the result (dry-run or commit). Paths
 * are double-quoted — safe for /bin/sh and cmd.exe alike. */
#include "drive2iso/isobuild.h"
#include "drive2iso/util.h"

#include <string.h>

char *isobuild_command(const char *stage_dir, const char *out_iso,
                       const char *label, const char *style,
                       bool isohybrid, const iso_tools_t *tools) {
    const char *hyb = isohybrid ? "-isohybrid-gpt-basdat" : "";

    if (style && strcmp(style, "windows") == 0) {
        /* Windows install/WinPE media: BIOS via etfsboot.com, UEFI via efisys.bin. */
        if (tools->have_oscdimg)
            /* oscdimg wants native backslash host paths (the Windows backend
             * passes them already normalized). */
            return str_format(
                "oscdimg -m -o -u2 -udfver102 -l\"%s\" "
                "-bootdata:2#p0,e,b\"%s\\boot\\etfsboot.com\""
                "#pEF,e,b\"%s\\efi\\microsoft\\boot\\efisys.bin\" \"%s\" \"%s\"",
                label, stage_dir, stage_dir, stage_dir, out_iso);
        if (tools->have_xorriso)
            return str_format(
                "xorriso -as mkisofs -iso-level 3 -J -joliet-long -D -volid \"%s\" "
                "-b boot/etfsboot.com -no-emul-boot -boot-load-size 8 "
                "-eltorito-alt-boot -e efi/microsoft/boot/efisys.bin -no-emul-boot "
                "%s -o \"%s\" \"%s\"",
                label, hyb, out_iso, stage_dir);
        return NULL;
    }

    /* Linux live ISO: GRUB covers BIOS (El Torito) + UEFI in one isohybrid image. */
    if (tools->have_grub_mkrescue)
        /* grub-mkrescue wraps xorriso; args after -- set the ISO volume id. */
        return str_format("grub-mkrescue --output=\"%s\" \"%s\" -- -volid \"%s\"",
                          out_iso, stage_dir, label);
    if (tools->have_xorriso)
        /* Fallback: stage must already carry grub's boot/grub/bios.img and
         * EFI/efiboot.img (built by provision_bootenv). */
        return str_format(
            "xorriso -as mkisofs -iso-level 3 -full-iso9660-filenames -volid \"%s\" "
            "-eltorito-boot boot/grub/bios.img -no-emul-boot -boot-load-size 4 "
            "-boot-info-table --grub2-boot-info "
            "-eltorito-alt-boot -e EFI/efiboot.img -no-emul-boot "
            "%s -o \"%s\" \"%s\"",
            label, hyb, out_iso, stage_dir);
    return NULL;
}
