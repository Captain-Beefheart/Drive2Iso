#include "winvss.h"
#include "drive2iso/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* diskshadow.exe ships only on Windows Server, so we drive VSS through the
 * Win32_ShadowCopy WMI class (present on every client edition) via PowerShell,
 * and expose the shadow as a directory symlink under %TEMP% that the capture
 * tools read like any folder. The shadow ID is kept for release. */
static char g_shadow_id[64];

static char *script_path(const char *name) {
    const char *tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = "C:\\Windows\\Temp";
    return str_format("%s\\%s", tmp, name);
}

/* Nth nonblank line of s into buf (0-based over nonblank lines). */
static void nth_nonblank(const char *s, int n, char *buf, size_t bufsz) {
    buf[0] = '\0';
    int seen = 0;
    while (s && *s) {
        const char *nl = strchr(s, '\n');
        size_t len = nl ? (size_t)(nl - s) : strlen(s);
        while (len && (s[len - 1] == '\r' || s[len - 1] == ' ')) --len;
        if (len) {
            if (seen == n) {
                size_t c = len < bufsz - 1 ? len : bufsz - 1;
                memcpy(buf, s, c); buf[c] = '\0';
                return;
            }
            ++seen;
        }
        if (!nl) break;
        s = nl + 1;
    }
}

int winvss_create(const char *volume, bool commit, char **out_exposed) {
    *out_exposed = NULL;
    g_shadow_id[0] = '\0';
    char letter = volume && volume[0] ? volume[0] : 'C';

    char *ps = str_format(
        "$ErrorActionPreference='Stop'\r\n"
        "try {\r\n"
        "  $r = (Get-WmiObject -List Win32_ShadowCopy).Create('%c:\\','ClientAccessible')\r\n"
        "  if ($r.ReturnValue -ne 0) { Write-Error ('VSS create failed: ' + $r.ReturnValue); exit 1 }\r\n"
        "  $sc = Get-WmiObject Win32_ShadowCopy | Where-Object { $_.ID -eq $r.ShadowID }\r\n"
        "  $dev = $sc.DeviceObject + '\\'\r\n"
        "  $link = Join-Path $env:TEMP 'd2i_vss'\r\n"
        "  if (Test-Path $link) { cmd /c rmdir \"$link\" 2>$null }\r\n"
        "  cmd /c mklink /d \"$link\" \"$dev\" | Out-Null\r\n"
        "  Write-Output $sc.ID\r\n"
        "  Write-Output $link\r\n"
        "} catch { Write-Error $_; exit 1 }\r\n", letter);
    if (!ps) return -1;

    if (!commit) {
        log_info("(dry-run) VSS snapshot of %c: via Win32_ShadowCopy, exposed under %%TEMP%%\\d2i_vss", letter);
        free(ps);
        return 0;
    }

    char *path = script_path("d2i_vss_create.ps1");
    int rc = -1;
    char *out = NULL;
    if (path && write_file(path, ps) == 0) {
        char *cmd = str_format("powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\"", path);
        if (cmd) { log_info("+ %s", cmd); out = run_capture(cmd); free(cmd); }
    }
    free(path); free(ps);

    if (out) {
        char id[64], link[512];
        nth_nonblank(out, 0, id, sizeof id);
        nth_nonblank(out, 1, link, sizeof link);
        free(out);
        if (link[0]) {
            snprintf(g_shadow_id, sizeof g_shadow_id, "%s", id);
            *out_exposed = xstrdup(link);
            log_info("VSS snapshot of %c: exposed at %s", letter, link);
            rc = 0;
        }
    }
    if (rc != 0) log_err("VSS snapshot failed (elevated prompt required)");
    return rc;
}

int winvss_release(const char *exposed, bool commit) {
    if (!exposed) return 0;
    if (!commit) { log_info("(dry-run) remove VSS symlink %s and delete the shadow", exposed); return 0; }

    char *ps = str_format(
        "$ErrorActionPreference='SilentlyContinue'\r\n"
        "cmd /c rmdir \"%s\" 2>$null\r\n"
        "$sc = Get-WmiObject Win32_ShadowCopy | Where-Object { $_.ID -eq '%s' }\r\n"
        "if ($sc) { $sc.Delete() }\r\n", exposed, g_shadow_id);
    char *path = script_path("d2i_vss_delete.ps1");
    int rc = -1;
    if (ps && path && write_file(path, ps) == 0) {
        char *cmd = str_format("powershell -NoProfile -ExecutionPolicy Bypass -File \"%s\"", path);
        if (cmd) { log_info("+ %s", cmd); rc = system(cmd); free(cmd); }
    }
    free(ps); free(path);
    if (rc != 0) log_warn("could not fully release the VSS shadow (may need manual cleanup)");
    return rc == 0 ? 0 : -1;
}
