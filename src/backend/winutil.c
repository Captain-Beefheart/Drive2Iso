#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00 /* Windows 10: GetFirmwareType, RegGetValue */
#endif

#include "winutil.h"
#include "drive2iso/util.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

bool win_is_elevated(void) {
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD sz = sizeof el;
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof el, &sz))
            r = el.TokenIsElevated != 0;
        CloseHandle(tok);
    }
    return r;
}

firmware_t win_firmware_type(void) {
    FIRMWARE_TYPE ft;
    if (GetFirmwareType(&ft)) {
        if (ft == FirmwareTypeUefi) return FW_UEFI;
        if (ft == FirmwareTypeBios) return FW_BIOS;
    }
    /* Fallback: a BIOS system rejects firmware-variable calls. */
    SetLastError(0);
    GetFirmwareEnvironmentVariableA("", "{00000000-0000-0000-0000-000000000000}", NULL, 0);
    if (GetLastError() == ERROR_INVALID_FUNCTION) return FW_BIOS;
    return FW_UNKNOWN;
}

const char *win_firmware_name(firmware_t f) {
    switch (f) {
        case FW_UEFI: return "UEFI";
        case FW_BIOS: return "Legacy BIOS";
        default:      return "unknown";
    }
}

int win_secure_boot_enabled(void) {
    DWORD val = 0, sz = sizeof val;
    LONG r = RegGetValueA(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
        "UEFISecureBootEnabled", RRF_RT_REG_DWORD, NULL, &val, &sz);
    if (r != ERROR_SUCCESS) return -1;
    return val ? 1 : 0;
}

int win_fast_startup_enabled(void) {
    DWORD val = 0, sz = sizeof val;
    LONG r = RegGetValueA(HKEY_LOCAL_MACHINE,
        "SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Power",
        "HiberbootEnabled", RRF_RT_REG_DWORD, NULL, &val, &sz);
    if (r != ERROR_SUCCESS) return -1;
    return val ? 1 : 0;
}

int win_bitlocker_on(char drive) {
    char cmd[64];
    snprintf(cmd, sizeof cmd, "manage-bde -status %c: 2>NUL", drive);
    char *out = run_capture(cmd);
    if (!out || !*out) { free(out); return -1; }
    int r = -1;
    if (strstr(out, "Protection On")) r = 1;
    else if (strstr(out, "Protection Off") || strstr(out, "Fully Decrypted")) r = 0;
    free(out);
    return r;
}

char win_free_drive_letter(void) {
    DWORD mask = GetLogicalDrives();
    for (char c = 'Z'; c >= 'D'; --c)
        if (!(mask & (1u << (c - 'A')))) return c;
    return 0;
}
