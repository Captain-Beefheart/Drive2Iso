#ifndef D2I_WINUTIL_H
#define D2I_WINUTIL_H

#include <stdbool.h>

/* Windows environment probes used by the Windows backend. All are read-only. */

typedef enum { FW_UNKNOWN = 0, FW_BIOS, FW_UEFI } firmware_t;

bool        win_is_elevated(void);          /* running as administrator? */
firmware_t  win_firmware_type(void);        /* how Windows itself booted */
const char *win_firmware_name(firmware_t f);
int         win_secure_boot_enabled(void);  /* 1 = on, 0 = off, -1 = unknown */
int         win_fast_startup_enabled(void); /* 1 = on, 0 = off, -1 = unknown */
int         win_bitlocker_on(char drive);   /* 1 = on, 0 = off, -1 = unknown */
char        win_free_drive_letter(void);    /* an unused letter, or 0 if none */

#endif /* D2I_WINUTIL_H */
