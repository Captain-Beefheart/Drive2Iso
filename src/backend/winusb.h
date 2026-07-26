#ifndef D2I_WINUSB_H
#define D2I_WINUSB_H

#include <stddef.h>
#include <stdint.h>

/* Raw physical-drive access for the bootable-USB writer. winusb_open locks and
 * dismounts the drive's volumes, then opens \\.\PhysicalDriveN for raw IO. */
typedef struct winusb_dev winusb_dev;

winusb_dev *winusb_open(int drive, char **err); /* *err set (caller frees) on failure */
long winusb_write (winusb_dev *d, const void *buf, size_t len);
long winusb_read  (winusb_dev *d, void *buf, size_t len);
int  winusb_rewind(winusb_dev *d);
void winusb_close (winusb_dev *d);

int      winusb_is_removable(int drive); /* 1 = removable, 0 = fixed, -1 = unknown */
int      winusb_hosts_system(int drive); /* 1 if it hosts the Windows system volume */
uint64_t winusb_size(int drive);         /* bytes, 0 if unknown */
void     winusb_list(void);              /* print all physical drives */

#endif /* D2I_WINUSB_H */
